// bin/enclave/sealed_storage.cpp
// AES-256-GCM sealed storage for straylight-enclave.
// OpenSSL EVP is the crypto backend; SGX EGETKEY path is gated behind
// STRAYLIGHT_SGX_HW compile-time define.

#include "sealed_storage.h"

#include <straylight/log.h>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/err.h>

#include <cassert>
#include <cstring>
#include <memory>

namespace straylight::enclave {

namespace {

/// RAII wrapper for EVP_CIPHER_CTX.
struct EvpCtxDeleter {
    void operator()(EVP_CIPHER_CTX* p) const noexcept {
        if (p) EVP_CIPHER_CTX_free(p);
    }
};
using EvpCtxPtr = std::unique_ptr<EVP_CIPHER_CTX, EvpCtxDeleter>;

/// Collect the last OpenSSL error string.
std::string openssl_error() {
    unsigned long e = ERR_get_error();
    if (e == 0) return "unknown OpenSSL error";
    char buf[256]{};
    ERR_error_string_n(e, buf, sizeof(buf));
    return std::string(buf);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Key derivation
// ---------------------------------------------------------------------------

void SealedStore::derive_stub_key() {
    // Deterministic 32-byte key for reproducible test behaviour.
    enclave_key_.assign(32, static_cast<uint8_t>(0xAB));
}

Result<void, std::string> SealedStore::derive_hw_key(SealPolicy /*policy*/) {
#ifdef STRAYLIGHT_SGX_HW
    // In real SGX: call sgx_get_key() with KEYPOLICY_MRENCLAVE or KEYPOLICY_MRSIGNER
    // depending on policy, using the current enclave's KEYNAME_SEAL_KEY.
    // This requires the call to originate from within a trusted enclave context.
    return Result<void, std::string>::error(
        "derive_hw_key: SGX EGETKEY ECall not wired — use stub mode");
#else
    return Result<void, std::string>::error("SGX hardware not compiled in");
#endif
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

Result<void, std::string> SealedStore::init(SgxMode mode) {
    mode_ = mode;
    if (mode == SgxMode::Hardware) {
        auto r = derive_hw_key(SealPolicy::MrEnclave);
        if (!r.has_value()) {
            return r;
        }
    } else {
        derive_stub_key();
    }
    initialized_ = true;
    SL_DEBUG("SealedStore initialized mode={}",
             mode == SgxMode::Hardware ? "hardware" : "stub");
    return Result<void, std::string>::ok();
}

Result<SealedBlob, std::string>
SealedStore::seal(std::span<const uint8_t> plaintext, SealPolicy policy) {
    if (!initialized_) {
        return Result<SealedBlob, std::string>::error("SealedStore not initialized");
    }
    assert(enclave_key_.size() == 32);

    SealedBlob blob;
    blob.policy = policy;

    // Generate a fresh 12-byte random nonce per NIST SP 800-38D recommendation.
    blob.nonce.resize(12);
    if (RAND_bytes(blob.nonce.data(), static_cast<int>(blob.nonce.size())) != 1) {
        return Result<SealedBlob, std::string>::error(
            "RAND_bytes failed: " + openssl_error());
    }

    // Allocate output buffers.
    blob.ciphertext.resize(plaintext.size());
    blob.tag.resize(16);

    EvpCtxPtr ctx(EVP_CIPHER_CTX_new());
    if (!ctx) {
        return Result<SealedBlob, std::string>::error(
            "EVP_CIPHER_CTX_new failed: " + openssl_error());
    }

    if (EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr,
                           enclave_key_.data(), blob.nonce.data()) != 1) {
        return Result<SealedBlob, std::string>::error(
            "EVP_EncryptInit_ex failed: " + openssl_error());
    }

    int out_len = 0;
    if (!plaintext.empty()) {
        if (EVP_EncryptUpdate(ctx.get(),
                              blob.ciphertext.data(), &out_len,
                              plaintext.data(),
                              static_cast<int>(plaintext.size())) != 1) {
            return Result<SealedBlob, std::string>::error(
                "EVP_EncryptUpdate failed: " + openssl_error());
        }
    }

    int final_len = 0;
    if (EVP_EncryptFinal_ex(ctx.get(),
                            blob.ciphertext.data() + out_len, &final_len) != 1) {
        return Result<SealedBlob, std::string>::error(
            "EVP_EncryptFinal_ex failed: " + openssl_error());
    }
    blob.ciphertext.resize(static_cast<size_t>(out_len + final_len));

    // Extract the 16-byte GCM authentication tag.
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG,
                            static_cast<int>(blob.tag.size()),
                            blob.tag.data()) != 1) {
        return Result<SealedBlob, std::string>::error(
            "EVP_CTRL_GCM_GET_TAG failed: " + openssl_error());
    }

    return Result<SealedBlob, std::string>::ok(std::move(blob));
}

Result<std::vector<uint8_t>, std::string>
SealedStore::unseal(const SealedBlob& blob) {
    if (!initialized_) {
        return Result<std::vector<uint8_t>, std::string>::error(
            "SealedStore not initialized");
    }
    if (blob.nonce.size() != 12) {
        return Result<std::vector<uint8_t>, std::string>::error(
            "Invalid nonce length: expected 12, got " +
            std::to_string(blob.nonce.size()));
    }
    if (blob.tag.size() != 16) {
        return Result<std::vector<uint8_t>, std::string>::error(
            "Invalid tag length: expected 16, got " +
            std::to_string(blob.tag.size()));
    }
    assert(enclave_key_.size() == 32);

    std::vector<uint8_t> plaintext(blob.ciphertext.size());

    EvpCtxPtr ctx(EVP_CIPHER_CTX_new());
    if (!ctx) {
        return Result<std::vector<uint8_t>, std::string>::error(
            "EVP_CIPHER_CTX_new failed: " + openssl_error());
    }

    if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr,
                           enclave_key_.data(), blob.nonce.data()) != 1) {
        return Result<std::vector<uint8_t>, std::string>::error(
            "EVP_DecryptInit_ex failed: " + openssl_error());
    }

    // Set the expected tag before final decryption.
    // EVP_CTRL_GCM_SET_TAG requires a non-const pointer.
    std::vector<uint8_t> tag_copy = blob.tag;
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG,
                            static_cast<int>(tag_copy.size()),
                            tag_copy.data()) != 1) {
        return Result<std::vector<uint8_t>, std::string>::error(
            "EVP_CTRL_GCM_SET_TAG failed: " + openssl_error());
    }

    int out_len = 0;
    if (!blob.ciphertext.empty()) {
        if (EVP_DecryptUpdate(ctx.get(),
                              plaintext.data(), &out_len,
                              blob.ciphertext.data(),
                              static_cast<int>(blob.ciphertext.size())) != 1) {
            return Result<std::vector<uint8_t>, std::string>::error(
                "EVP_DecryptUpdate failed: " + openssl_error());
        }
    }

    int final_len = 0;
    if (EVP_DecryptFinal_ex(ctx.get(),
                            plaintext.data() + out_len, &final_len) <= 0) {
        // GCM tag mismatch — data is tampered or sealed with a different key.
        return Result<std::vector<uint8_t>, std::string>::error(
            "Authentication tag mismatch — data tampered or wrong enclave identity");
    }
    plaintext.resize(static_cast<size_t>(out_len + final_len));

    return Result<std::vector<uint8_t>, std::string>::ok(std::move(plaintext));
}

} // namespace straylight::enclave
