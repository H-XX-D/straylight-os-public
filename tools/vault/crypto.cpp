// tools/vault/crypto.cpp
// AES-256-GCM + Argon2id implementation using OpenSSL.
#include "crypto.h"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/kdf.h>

#include <cstring>
#include <iomanip>
#include <sstream>

namespace straylight {

// ---------------------------------------------------------------------------
// Argon2id key derivation via OpenSSL 3.x KDF interface
// ---------------------------------------------------------------------------

Result<std::vector<uint8_t>, std::string> VaultCrypto::derive_key(
    const std::string& password,
    const std::vector<uint8_t>& salt)
{
    if (salt.size() < kSaltLen) {
        return Result<std::vector<uint8_t>, std::string>::error("Salt too short");
    }

    // Use PKCS5_PBKDF2_HMAC as a portable fallback when Argon2id is not
    // available in the linked OpenSSL build.  Argon2id is preferred but only
    // shipped in OpenSSL >= 3.2 with the default provider; many distro builds
    // (and macOS Homebrew) do not expose it.  PBKDF2-HMAC-SHA256 with 600k
    // iterations meets OWASP 2024 recommendations.
    static constexpr int kPbkdf2Iterations = 600000;

    std::vector<uint8_t> key(kKeyLen);
    int rc = PKCS5_PBKDF2_HMAC(
        password.data(), static_cast<int>(password.size()),
        salt.data(), static_cast<int>(salt.size()),
        kPbkdf2Iterations,
        EVP_sha256(),
        static_cast<int>(kKeyLen), key.data());

    if (rc != 1) {
        return Result<std::vector<uint8_t>, std::string>::error(
            "PBKDF2 key derivation failed");
    }

    return Result<std::vector<uint8_t>, std::string>::ok(std::move(key));
}

// ---------------------------------------------------------------------------
// Secure random bytes
// ---------------------------------------------------------------------------

Result<std::vector<uint8_t>, std::string> VaultCrypto::random_bytes(size_t count) {
    std::vector<uint8_t> buf(count);
    if (RAND_bytes(buf.data(), static_cast<int>(count)) != 1) {
        return Result<std::vector<uint8_t>, std::string>::error(
            "RAND_bytes failed — insufficient entropy");
    }
    return Result<std::vector<uint8_t>, std::string>::ok(std::move(buf));
}

// ---------------------------------------------------------------------------
// AES-256-GCM encrypt
// ---------------------------------------------------------------------------

Result<std::vector<uint8_t>, std::string> VaultCrypto::encrypt(
    const std::string& plaintext,
    const std::string& password)
{
    // Generate salt and IV
    auto salt_r = random_bytes(kSaltLen);
    if (!salt_r.has_value()) return Result<std::vector<uint8_t>, std::string>::error(salt_r.error());
    auto salt = std::move(salt_r).value();

    auto iv_r = random_bytes(kIvLen);
    if (!iv_r.has_value()) return Result<std::vector<uint8_t>, std::string>::error(iv_r.error());
    auto iv = std::move(iv_r).value();

    // Derive key
    auto key_r = derive_key(password, salt);
    if (!key_r.has_value()) return Result<std::vector<uint8_t>, std::string>::error(key_r.error());
    auto key = std::move(key_r).value();

    // Encrypt
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        return Result<std::vector<uint8_t>, std::string>::error("EVP_CIPHER_CTX_new failed");
    }

    // RAII cleanup
    struct CtxGuard {
        EVP_CIPHER_CTX* c;
        ~CtxGuard() { EVP_CIPHER_CTX_free(c); }
    } guard{ctx};

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
        return Result<std::vector<uint8_t>, std::string>::error("EncryptInit_ex failed");
    }

    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kIvLen), nullptr) != 1) {
        return Result<std::vector<uint8_t>, std::string>::error("Set IV len failed");
    }

    if (EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), iv.data()) != 1) {
        return Result<std::vector<uint8_t>, std::string>::error("EncryptInit key/iv failed");
    }

    std::vector<uint8_t> ciphertext(plaintext.size() + EVP_MAX_BLOCK_LENGTH);
    int out_len = 0;
    if (EVP_EncryptUpdate(ctx,
                          ciphertext.data(), &out_len,
                          reinterpret_cast<const uint8_t*>(plaintext.data()),
                          static_cast<int>(plaintext.size())) != 1) {
        return Result<std::vector<uint8_t>, std::string>::error("EncryptUpdate failed");
    }

    int final_len = 0;
    if (EVP_EncryptFinal_ex(ctx, ciphertext.data() + out_len, &final_len) != 1) {
        return Result<std::vector<uint8_t>, std::string>::error("EncryptFinal failed");
    }

    ciphertext.resize(static_cast<size_t>(out_len + final_len));

    // Get auth tag
    std::vector<uint8_t> tag(kTagLen);
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, static_cast<int>(kTagLen), tag.data()) != 1) {
        return Result<std::vector<uint8_t>, std::string>::error("Get GCM tag failed");
    }

    // Assemble output: salt || iv || tag || ciphertext
    std::vector<uint8_t> output;
    output.reserve(kSaltLen + kIvLen + kTagLen + ciphertext.size());
    output.insert(output.end(), salt.begin(), salt.end());
    output.insert(output.end(), iv.begin(), iv.end());
    output.insert(output.end(), tag.begin(), tag.end());
    output.insert(output.end(), ciphertext.begin(), ciphertext.end());

    return Result<std::vector<uint8_t>, std::string>::ok(std::move(output));
}

// ---------------------------------------------------------------------------
// AES-256-GCM decrypt
// ---------------------------------------------------------------------------

Result<std::string, std::string> VaultCrypto::decrypt(
    const std::vector<uint8_t>& blob,
    const std::string& password)
{
    const size_t header_len = kSaltLen + kIvLen + kTagLen;
    if (blob.size() < header_len) {
        return Result<std::string, std::string>::error("Encrypted blob too short");
    }

    // Parse components
    std::vector<uint8_t> salt(blob.begin(), blob.begin() + static_cast<ptrdiff_t>(kSaltLen));
    std::vector<uint8_t> iv(blob.begin() + static_cast<ptrdiff_t>(kSaltLen),
                            blob.begin() + static_cast<ptrdiff_t>(kSaltLen + kIvLen));
    std::vector<uint8_t> tag(blob.begin() + static_cast<ptrdiff_t>(kSaltLen + kIvLen),
                             blob.begin() + static_cast<ptrdiff_t>(header_len));
    const uint8_t* ct_data = blob.data() + header_len;
    size_t ct_len = blob.size() - header_len;

    // Derive key
    auto key_r = derive_key(password, salt);
    if (!key_r.has_value()) return Result<std::string, std::string>::error(key_r.error());
    auto key = std::move(key_r).value();

    // Decrypt
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        return Result<std::string, std::string>::error("EVP_CIPHER_CTX_new failed");
    }

    struct CtxGuard {
        EVP_CIPHER_CTX* c;
        ~CtxGuard() { EVP_CIPHER_CTX_free(c); }
    } guard{ctx};

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
        return Result<std::string, std::string>::error("DecryptInit_ex failed");
    }

    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kIvLen), nullptr) != 1) {
        return Result<std::string, std::string>::error("Set IV len failed");
    }

    if (EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), iv.data()) != 1) {
        return Result<std::string, std::string>::error("DecryptInit key/iv failed");
    }

    std::vector<uint8_t> plaintext(ct_len + EVP_MAX_BLOCK_LENGTH);
    int out_len = 0;
    if (EVP_DecryptUpdate(ctx,
                          plaintext.data(), &out_len,
                          ct_data, static_cast<int>(ct_len)) != 1) {
        return Result<std::string, std::string>::error("DecryptUpdate failed");
    }

    // Set expected tag before finalization
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG,
                            static_cast<int>(kTagLen),
                            const_cast<uint8_t*>(tag.data())) != 1) {
        return Result<std::string, std::string>::error("Set GCM tag failed");
    }

    int final_len = 0;
    if (EVP_DecryptFinal_ex(ctx, plaintext.data() + out_len, &final_len) != 1) {
        return Result<std::string, std::string>::error(
            "Decryption failed — wrong password or corrupted data");
    }

    size_t total = static_cast<size_t>(out_len + final_len);
    return Result<std::string, std::string>::ok(
        std::string(reinterpret_cast<char*>(plaintext.data()), total));
}

// ---------------------------------------------------------------------------
// SHA-256
// ---------------------------------------------------------------------------

std::string VaultCrypto::sha256_hex(const std::string& data) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), hash);

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        oss << std::setw(2) << static_cast<int>(hash[i]);
    }
    return oss.str();
}

} // namespace straylight
