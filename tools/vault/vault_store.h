// tools/vault/vault_store.h
// Encrypted secret store backed by a JSON database file.
#pragma once

#include <straylight/result.h>
#include "crypto.h"

#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace straylight {

/// A single secret entry in the vault.
struct VaultEntry {
    std::string key;
    std::string value;
    std::string created_at;  // ISO 8601
    std::string updated_at;  // ISO 8601
};

/// Encrypted secret store for StrayLight OS.
///
/// Secrets are stored in a JSON file encrypted with AES-256-GCM.  The master
/// password is used to derive the encryption key via Argon2id (PBKDF2 fallback).
/// The store is locked by default and must be unlocked with the master password
/// before any operations can be performed.
class VaultStore {
public:
    /// Default vault database path.
    static std::string default_path();

    /// Create a new vault with the given master password.
    /// Fails if the vault file already exists.
    Result<void, std::string> create(const std::string& path,
                                      const std::string& master_password);

    /// Open an existing vault file.  Does not decrypt until unlock().
    Result<void, std::string> open(const std::string& path);

    /// Unlock the vault with the master password.
    Result<void, std::string> unlock(const std::string& master_password);

    /// Lock the vault, clearing all decrypted data from memory.
    void lock();

    /// Check if vault is currently unlocked.
    [[nodiscard]] bool is_unlocked() const;

    /// Set a secret by key path (e.g. "wifi/home/password").
    Result<void, std::string> set(const std::string& key, const std::string& value);

    /// Get a secret by key path.
    Result<std::string, std::string> get(const std::string& key) const;

    /// Delete a secret by key path.
    Result<void, std::string> del(const std::string& key);

    /// List all key paths, optionally filtered by prefix.
    std::vector<std::string> list(const std::string& prefix = "") const;

    /// Export the vault as an encrypted JSON blob.
    Result<std::vector<uint8_t>, std::string> export_backup(
        const std::string& export_password) const;

    /// Import secrets from an encrypted JSON blob.
    Result<int, std::string> import_backup(
        const std::vector<uint8_t>& blob,
        const std::string& export_password);

    /// Change the master password.
    Result<void, std::string> change_password(const std::string& old_password,
                                               const std::string& new_password);

    /// Auto-lock after idle timeout.
    void check_idle_timeout(std::chrono::seconds timeout);

    /// Reset the idle timer (called on every access).
    void touch();

private:
    /// Persist the vault to disk (encrypts and writes).
    Result<void, std::string> save();

    /// ISO 8601 timestamp.
    static std::string now_iso8601();

    mutable std::mutex mu_;
    std::string path_;
    std::string master_password_;
    std::vector<uint8_t> raw_encrypted_;
    std::map<std::string, VaultEntry> entries_;
    bool unlocked_ = false;
    std::chrono::steady_clock::time_point last_access_;
};

} // namespace straylight
