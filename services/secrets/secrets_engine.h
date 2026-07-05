// services/secrets/secrets_engine.h
// System-wide secrets manager — IPC-based, ACL-controlled, encrypted storage.
#pragma once

#include <straylight/result.h>
#include <straylight/error.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace straylight {

/// Access control entry for a secret.
struct SecretACL {
    std::set<uint32_t> allowed_uids;
    std::set<uint32_t> allowed_gids;
};

/// A stored secret with metadata.
struct SecretEntry {
    std::string key;
    std::string encrypted_value;
    SecretACL acl;
    std::chrono::system_clock::time_point created;
    std::chrono::system_clock::time_point last_accessed;
    std::chrono::system_clock::time_point last_rotated;
    int rotation_interval_hours = 0; // 0 = no auto-rotation
    int version = 1;
};

/// Audit log entry.
struct AuditEntry {
    std::chrono::system_clock::time_point timestamp;
    std::string action; // "get", "set", "delete", "rotate", "acl_change"
    std::string key;
    uint32_t uid;
    bool allowed;
    std::string detail;
};

class SecretsEngine {
public:
    SecretsEngine() = default;

    /// Initialize — load secrets store and encryption key.
    Result<void, SLError> init(const std::filesystem::path& store_path,
                                const std::string& master_key_path);

    /// Set a secret value (creates or updates).
    Result<void, SLError> set(const std::string& key, const std::string& value,
                               uint32_t caller_uid);

    /// Get a secret value.
    Result<std::string, SLError> get(const std::string& key, uint32_t caller_uid);

    /// Delete a secret.
    Result<void, SLError> remove(const std::string& key, uint32_t caller_uid);

    /// List all secret keys (filtered by ACL).
    std::vector<std::string> list(uint32_t caller_uid) const;

    /// Rotate a secret — generates a new value.
    Result<void, SLError> rotate(const std::string& key, uint32_t caller_uid);

    /// Add a UID to a secret's ACL.
    Result<void, SLError> acl_add(const std::string& key, uint32_t target_uid,
                                    uint32_t caller_uid);

    /// Remove a UID from a secret's ACL.
    Result<void, SLError> acl_remove(const std::string& key, uint32_t target_uid,
                                      uint32_t caller_uid);

    /// Get environment variables for a child process.
    std::map<std::string, std::string> get_env_for(uint32_t uid) const;

    /// Check for secrets needing rotation.
    Result<void, SLError> check_rotations();

    /// Get audit log.
    std::vector<AuditEntry> get_audit_log(int last_n = 100) const;

private:
    /// Derive an AES-256 key from the local master key.
    std::array<unsigned char, 32> derived_key() const;

    /// Encrypt a plaintext value.
    std::string encrypt(const std::string& plaintext) const;

    /// Decrypt an encrypted value.
    std::string decrypt(const std::string& ciphertext) const;

    /// Check if a UID has access to a key.
    bool check_access(const SecretEntry& entry, uint32_t uid) const;

    /// Log an audit entry.
    void audit(const std::string& action, const std::string& key,
               uint32_t uid, bool allowed, const std::string& detail = "");

    /// Save the secrets store to disk.
    Result<void, SLError> save() const;

    /// Load the secrets store from disk.
    void load();

    /// Generate a random secret value.
    std::string generate_random_value(int length = 32) const;

    mutable std::mutex mutex_;
    std::filesystem::path store_path_;
    std::string master_key_;
    std::map<std::string, SecretEntry> secrets_;
    std::vector<AuditEntry> audit_log_;
};

} // namespace straylight
