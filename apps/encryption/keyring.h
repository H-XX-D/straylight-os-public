// apps/encryption/keyring.h
// Named key storage sealed with crypto_secretbox under a master passphrase
#pragma once

#include "crypto.h"

#include <chrono>
#include <string>
#include <vector>

namespace straylight::encryption {

/// A single named key entry stored in the keyring.
/// The actual derived key bytes are encrypted under the master key using
/// crypto_secretbox_easy before being serialised to disk.
struct KeyEntry {
    std::string name;
    std::string description;
    std::vector<uint8_t> encrypted_key; // crypto_secretbox ciphertext
    std::vector<uint8_t> nonce;         // crypto_secretbox_NONCEBYTES
    std::vector<uint8_t> salt;          // Argon2id salt used to derive this entry's key
    std::chrono::system_clock::time_point created;
};

/// In-memory keyring backed by `~/.config/straylight/keyring.json`.
/// All entries are sealed under a master passphrase so the JSON file on disk
/// never contains plaintext key material.
class Keyring {
public:
    /// Load and decrypt the keyring from disk using `master_pass`.
    Result<void, SLError> load(std::string_view master_pass);

    /// Encrypt and write the keyring to disk.
    Result<void, SLError> save() const;

    /// Add a new named key derived from `pass`.
    Result<void, SLError> add(const std::string& name,
                               const std::string& description,
                               std::string_view pass);

    /// Remove the entry with the given name.
    Result<void, SLError> remove(const std::string& name);

    /// Return the DerivedKey for a named entry (requires the keyring to be unlocked).
    Result<DerivedKey, SLError> unlock(const std::string& name) const;

    /// Read-only view of all entries.
    [[nodiscard]] const std::vector<KeyEntry>& entries() const { return entries_; }

    [[nodiscard]] bool is_unlocked() const { return unlocked_; }

private:
    std::vector<KeyEntry> entries_;
    DerivedKey master_key_;
    bool unlocked_ = false;

    [[nodiscard]] static fs::path keyring_path();
};

} // namespace straylight::encryption
