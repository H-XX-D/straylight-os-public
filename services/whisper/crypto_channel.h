// services/whisper/crypto_channel.h
// X25519 key exchange and ChaCha20-Poly1305 encryption for Whisper channels.
#pragma once

#include <straylight/result.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace straylight {

/// Key material for a single direction of a Whisper channel.
struct KeyMaterial {
    std::array<uint8_t, 32> shared_secret;
    std::array<uint8_t, 32> private_key;
    std::array<uint8_t, 32> public_key;
    std::array<uint8_t, 32> peer_public_key;
    uint64_t message_counter = 0;
    uint64_t rotation_threshold = 1000; // Rotate key every N messages.
};

/// Encrypted payload with nonce.
struct EncryptedPayload {
    std::vector<uint8_t> ciphertext; // Includes 16-byte Poly1305 tag appended.
    std::array<uint8_t, 12> nonce;
};

/// Provides X25519 ECDH key exchange and ChaCha20-Poly1305 AEAD encryption.
/// Uses raw system calls to /dev/urandom for RNG and a bundled TweetNaCl-derived
/// implementation to avoid external crypto library dependencies.
class CryptoChannel {
public:
    CryptoChannel();
    ~CryptoChannel();

    /// Generate a fresh X25519 keypair.
    Result<void, std::string> generate_keypair();

    /// Get our public key (to send to peer).
    const std::array<uint8_t, 32>& public_key() const { return key_.public_key; }

    /// Perform key exchange with peer's public key, deriving the shared secret.
    Result<void, std::string> key_exchange(const std::array<uint8_t, 32>& peer_pub);

    /// Encrypt plaintext using ChaCha20-Poly1305 with the shared secret.
    Result<EncryptedPayload, std::string> encrypt(const std::string& plaintext);

    /// Decrypt ciphertext+nonce using ChaCha20-Poly1305.
    Result<std::string, std::string> decrypt(const std::vector<uint8_t>& ciphertext,
                                              const std::array<uint8_t, 12>& nonce);

    /// Check if key rotation is needed (returns true if counter >= threshold).
    bool needs_rotation() const;

    /// Rotate the key by hashing (shared_secret || counter) into a new secret.
    Result<void, std::string> rotate_key();

    /// Set the rotation threshold.
    void set_rotation_threshold(uint64_t n) { key_.rotation_threshold = n; }

    /// Get current message counter.
    uint64_t message_counter() const { return key_.message_counter; }

private:
    KeyMaterial key_;
    bool has_secret_ = false;

    /// Read random bytes from /dev/urandom.
    Result<void, std::string> random_bytes(uint8_t* buf, size_t len);

    /// X25519 scalar multiplication (Curve25519).
    static void x25519(uint8_t out[32], const uint8_t scalar[32],
                       const uint8_t point[32]);

    /// ChaCha20 block function.
    static void chacha20_block(uint32_t out[16], const uint32_t in[16]);

    /// ChaCha20 stream cipher.
    static void chacha20(uint8_t* out, const uint8_t* in, size_t len,
                         const uint8_t key[32], const uint8_t nonce[12],
                         uint32_t counter);

    /// Poly1305 MAC.
    static void poly1305(uint8_t tag[16], const uint8_t* msg, size_t len,
                         const uint8_t key[32]);

    /// AEAD construction: ChaCha20-Poly1305 (RFC 8439).
    static Result<std::vector<uint8_t>, std::string>
    aead_encrypt(const uint8_t* plaintext, size_t pt_len,
                 const uint8_t* aad, size_t aad_len,
                 const uint8_t key[32], const uint8_t nonce[12]);

    static Result<std::vector<uint8_t>, std::string>
    aead_decrypt(const uint8_t* ciphertext, size_t ct_len,
                 const uint8_t* aad, size_t aad_len,
                 const uint8_t key[32], const uint8_t nonce[12]);

    /// Simple SHA-256 for key derivation / rotation.
    static void sha256(uint8_t out[32], const uint8_t* data, size_t len);
};

} // namespace straylight
