// apps/encryption/crypto.cpp
// libsodium XChaCha20-Poly1305 streaming encrypt/decrypt
#include "crypto.h"

#include <sodium.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <vector>

namespace straylight::encryption {

namespace {
// 64 KiB plaintext chunks
constexpr size_t CHUNK_SIZE = 65536;

// sodium constants (validated to match libsodium 1.0.18 values at compile time)
// crypto_secretstream_xchacha20poly1305_HEADERBYTES == 24
// crypto_secretstream_xchacha20poly1305_ABYTES      == 17
// crypto_pwhash_SALTBYTES                            == 16

inline SLError make_err(SLErrorCode code, const std::string& msg) {
    return SLError{code, msg};
}
} // namespace

Result<void, SLError> Crypto::init() {
    if (sodium_init() < 0) {
        return Result<void, SLError>::error(
            make_err(SLErrorCode::Internal, "sodium_init() failed"));
    }
    return Result<void, SLError>::ok();
}

Result<DerivedKey, SLError> Crypto::derive_key(std::string_view passphrase,
                                                const uint8_t* salt) {
    DerivedKey dk{};

    if (salt) {
        std::memcpy(dk.salt.data(), salt, crypto_pwhash_SALTBYTES);
    } else {
        randombytes_buf(dk.salt.data(), crypto_pwhash_SALTBYTES);
    }

    int rc = crypto_pwhash(
        dk.key.data(),
        static_cast<unsigned long long>(dk.key.size()),
        passphrase.data(),
        static_cast<unsigned long long>(passphrase.size()),
        dk.salt.data(),
        crypto_pwhash_OPSLIMIT_MODERATE,
        crypto_pwhash_MEMLIMIT_MODERATE,
        crypto_pwhash_ALG_ARGON2ID13);

    if (rc != 0) {
        return Result<DerivedKey, SLError>::error(
            make_err(SLErrorCode::Internal,
                     "Argon2id key derivation failed (out of memory)"));
    }

    return Result<DerivedKey, SLError>::ok(dk);
}

Result<void, SLError> Crypto::encrypt_file(
    const fs::path& in,
    const fs::path& out,
    const DerivedKey& key,
    std::function<void(uint64_t, uint64_t)> progress)
{
    std::error_code ec;
    const uint64_t total = fs::is_regular_file(in, ec) ? fs::file_size(in, ec) : 0;

    std::ifstream fi(in, std::ios::binary);
    if (!fi) {
        return Result<void, SLError>::error(
            make_err(SLErrorCode::IOError, "Cannot open input: " + in.string()));
    }

    std::ofstream fo(out, std::ios::binary);
    if (!fo) {
        return Result<void, SLError>::error(
            make_err(SLErrorCode::IOError, "Cannot create output: " + out.string()));
    }

    // Write 16-byte salt header
    fo.write(reinterpret_cast<const char*>(key.salt.data()),
             static_cast<std::streamsize>(crypto_pwhash_SALTBYTES));

    // Initialise stream push state and write 24-byte header
    crypto_secretstream_xchacha20poly1305_state st{};
    uint8_t stream_header[crypto_secretstream_xchacha20poly1305_HEADERBYTES];
    crypto_secretstream_xchacha20poly1305_init_push(&st, stream_header, key.key.data());
    fo.write(reinterpret_cast<const char*>(stream_header),
             crypto_secretstream_xchacha20poly1305_HEADERBYTES);

    std::vector<uint8_t> plain(CHUNK_SIZE);
    std::vector<uint8_t> cipher(CHUNK_SIZE + crypto_secretstream_xchacha20poly1305_ABYTES);

    uint64_t done = 0;

    while (fi) {
        fi.read(reinterpret_cast<char*>(plain.data()),
                static_cast<std::streamsize>(CHUNK_SIZE));
        const auto n = static_cast<size_t>(fi.gcount());
        if (n == 0) break;

        // TAG_FINAL marks the last chunk so decryption can verify completeness
        const uint8_t tag = (fi.peek() == EOF)
            ? crypto_secretstream_xchacha20poly1305_TAG_FINAL
            : crypto_secretstream_xchacha20poly1305_TAG_MESSAGE;

        unsigned long long cipher_len = 0;
        crypto_secretstream_xchacha20poly1305_push(
            &st,
            cipher.data(), &cipher_len,
            plain.data(), static_cast<unsigned long long>(n),
            nullptr, 0,
            tag);

        fo.write(reinterpret_cast<const char*>(cipher.data()),
                 static_cast<std::streamsize>(cipher_len));

        done += n;
        if (progress) progress(done, total);
    }

    if (!fo) {
        return Result<void, SLError>::error(
            make_err(SLErrorCode::IOError, "Write error on output: " + out.string()));
    }

    return Result<void, SLError>::ok();
}

Result<void, SLError> Crypto::decrypt_file(
    const fs::path& in,
    const fs::path& out,
    std::string_view passphrase,
    std::function<void(uint64_t, uint64_t)> progress)
{
    std::error_code ec;
    const uint64_t total = fs::is_regular_file(in, ec) ? fs::file_size(in, ec) : 0;

    std::ifstream fi(in, std::ios::binary);
    if (!fi) {
        return Result<void, SLError>::error(
            make_err(SLErrorCode::IOError, "Cannot open encrypted file: " + in.string()));
    }

    // Read embedded 16-byte salt
    uint8_t salt[crypto_pwhash_SALTBYTES];
    fi.read(reinterpret_cast<char*>(salt), crypto_pwhash_SALTBYTES);
    if (fi.gcount() != static_cast<std::streamsize>(crypto_pwhash_SALTBYTES)) {
        return Result<void, SLError>::error(
            make_err(SLErrorCode::ParseError, "Truncated file: missing salt"));
    }

    // Derive key from passphrase + embedded salt
    auto dk_res = derive_key(passphrase, salt);
    if (!dk_res.has_value()) {
        return Result<void, SLError>::error(dk_res.error());
    }
    const DerivedKey dk = dk_res.value();

    // Read 24-byte stream header
    uint8_t stream_header[crypto_secretstream_xchacha20poly1305_HEADERBYTES];
    fi.read(reinterpret_cast<char*>(stream_header),
            crypto_secretstream_xchacha20poly1305_HEADERBYTES);
    if (fi.gcount() != static_cast<std::streamsize>(
            crypto_secretstream_xchacha20poly1305_HEADERBYTES)) {
        return Result<void, SLError>::error(
            make_err(SLErrorCode::ParseError, "Truncated file: missing stream header"));
    }

    crypto_secretstream_xchacha20poly1305_state st{};
    if (crypto_secretstream_xchacha20poly1305_init_pull(&st, stream_header, dk.key.data()) != 0) {
        return Result<void, SLError>::error(
            make_err(SLErrorCode::Internal, "Stream header validation failed"));
    }

    std::ofstream fo(out, std::ios::binary);
    if (!fo) {
        return Result<void, SLError>::error(
            make_err(SLErrorCode::IOError, "Cannot create output: " + out.string()));
    }

    const size_t cipher_chunk = CHUNK_SIZE + crypto_secretstream_xchacha20poly1305_ABYTES;
    std::vector<uint8_t> cipher(cipher_chunk);
    std::vector<uint8_t> plain(CHUNK_SIZE);

    uint64_t done = 0;

    while (fi) {
        fi.read(reinterpret_cast<char*>(cipher.data()),
                static_cast<std::streamsize>(cipher_chunk));
        const auto n = static_cast<size_t>(fi.gcount());
        if (n == 0) break;

        unsigned long long plain_len = 0;
        uint8_t tag = 0;
        int rc = crypto_secretstream_xchacha20poly1305_pull(
            &st,
            plain.data(), &plain_len,
            &tag,
            cipher.data(), static_cast<unsigned long long>(n),
            nullptr, 0);

        if (rc != 0) {
            fo.close();
            fs::remove(out, ec);
            return Result<void, SLError>::error(
                make_err(SLErrorCode::Internal,
                         "Decryption failed: invalid ciphertext or wrong passphrase"));
        }

        fo.write(reinterpret_cast<const char*>(plain.data()),
                 static_cast<std::streamsize>(plain_len));

        done += n;
        if (progress) progress(done, total);

        if (tag == crypto_secretstream_xchacha20poly1305_TAG_FINAL) break;
    }

    if (!fo) {
        return Result<void, SLError>::error(
            make_err(SLErrorCode::IOError, "Write error on output: " + out.string()));
    }

    return Result<void, SLError>::ok();
}

} // namespace straylight::encryption
