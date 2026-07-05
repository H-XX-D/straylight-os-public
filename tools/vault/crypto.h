// tools/vault/crypto.h
// AES-256-GCM encryption with Argon2id key derivation for StrayLight Vault.
#pragma once

#include <straylight/result.h>

#include <cstdint>
#include <string>
#include <vector>

namespace straylight {

/// Cryptographic primitives for the vault secret store.
/// Uses AES-256-GCM for authenticated encryption and Argon2id for key derivation.
class VaultCrypto {
public:
    static constexpr size_t kKeyLen = 32;         // AES-256
    static constexpr size_t kIvLen = 12;          // GCM nonce
    static constexpr size_t kTagLen = 16;         // GCM auth tag
    static constexpr size_t kSaltLen = 32;        // Argon2id salt
    static constexpr uint32_t kArgonTimeCost = 3;
    static constexpr uint32_t kArgonMemCost = 65536; // 64 MiB
    static constexpr uint32_t kArgonParallelism = 4;

    /// Derive an AES-256 key from a password using Argon2id.
    /// Returns the derived key bytes.
    static Result<std::vector<uint8_t>, std::string> derive_key(
        const std::string& password,
        const std::vector<uint8_t>& salt);

    /// Generate cryptographically secure random bytes.
    static Result<std::vector<uint8_t>, std::string> random_bytes(size_t count);

    /// Encrypt plaintext with AES-256-GCM.
    /// Returns: salt(32) || iv(12) || tag(16) || ciphertext
    static Result<std::vector<uint8_t>, std::string> encrypt(
        const std::string& plaintext,
        const std::string& password);

    /// Decrypt ciphertext produced by encrypt().
    /// Input format: salt(32) || iv(12) || tag(16) || ciphertext
    static Result<std::string, std::string> decrypt(
        const std::vector<uint8_t>& blob,
        const std::string& password);

    /// Compute SHA-256 hash of data, returned as hex string.
    static std::string sha256_hex(const std::string& data);
};

} // namespace straylight
