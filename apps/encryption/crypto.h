// apps/encryption/crypto.h
// XChaCha20-Poly1305 streaming encrypt/decrypt with Argon2id key derivation
#pragma once

#include <straylight/result.h>
#include <straylight/error.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string_view>

namespace straylight::encryption {

namespace fs = std::filesystem;

/// 32-byte session key + 16-byte Argon2id salt
struct DerivedKey {
    std::array<uint8_t, 32> key;  // crypto_secretstream_xchacha20poly1305_KEYBYTES
    std::array<uint8_t, 16> salt; // crypto_pwhash_SALTBYTES
};

/// Low-level cryptographic operations backed by libsodium.
class Crypto {
public:
    /// Must be called once at program start before any other Crypto functions.
    static Result<void, SLError> init();

    /// Derive a 32-byte key from a passphrase using Argon2id.
    /// If salt is nullptr, a random 16-byte salt is generated.
    static Result<DerivedKey, SLError> derive_key(std::string_view passphrase,
                                                   const uint8_t* salt = nullptr);

    /// Stream-encrypt `in` → `out`.
    /// File layout on disk: [salt:16][header:24][ciphertext chunks…]
    static Result<void, SLError> encrypt_file(
        const fs::path& in,
        const fs::path& out,
        const DerivedKey& key,
        std::function<void(uint64_t done, uint64_t total)> progress = {});

    /// Stream-decrypt `in` → `out`.
    /// Reads the embedded salt, derives the key from passphrase, then decrypts.
    static Result<void, SLError> decrypt_file(
        const fs::path& in,
        const fs::path& out,
        std::string_view passphrase,
        std::function<void(uint64_t done, uint64_t total)> progress = {});
};

} // namespace straylight::encryption
