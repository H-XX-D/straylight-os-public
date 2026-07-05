// tools/users/user_manager.h
// User account management for StrayLight OS.
#pragma once

#include <straylight/result.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace straylight {

/// Represents a system user account.
struct UserAccount {
    std::string username;
    uint32_t uid = 0;
    uint32_t gid = 0;
    std::string full_name;      // GECOS field
    std::string home_dir;
    std::string shell;
    std::vector<std::string> groups;
    bool locked = false;
    bool has_password = false;
    bool has_sudo = false;
    std::string last_login;
    std::string password_expires;
    int ssh_key_count = 0;
};

/// Login history entry.
struct LoginRecord {
    std::string username;
    std::string terminal;      // tty, pts/N
    std::string from_host;     // IP or hostname
    std::string login_time;
    std::string logout_time;
    std::string duration;
    bool still_logged_in = false;
};

/// SSH key info.
struct SshKey {
    std::string type;          // "ssh-rsa", "ssh-ed25519", etc.
    std::string fingerprint;
    std::string comment;
    std::string full_key;
};

/// Password policy.
struct PasswordPolicy {
    int min_length = 8;
    int max_days = 99999;
    int min_days = 0;
    int warn_days = 7;
    bool require_uppercase = false;
    bool require_lowercase = false;
    bool require_digits = false;
    bool require_special = false;
};

class UserManager {
public:
    UserManager();
    ~UserManager();

    /// List all user accounts (filtered by UID range).
    Result<std::vector<UserAccount>, std::string> list_users(
        bool include_system = false) const;

    /// Get detailed info for a specific user.
    Result<UserAccount, std::string> get_user(const std::string& username) const;

    /// Add a new user account.
    Result<void, std::string> add_user(const std::string& username,
                                        const std::string& shell = "/bin/bash",
                                        const std::string& home = "",
                                        const std::vector<std::string>& groups = {},
                                        const std::string& full_name = "",
                                        bool create_home = true);

    /// Delete a user account.
    Result<void, std::string> delete_user(const std::string& username,
                                            bool remove_home = false);

    /// Modify user properties.
    Result<void, std::string> modify_user(const std::string& username,
                                            const std::string& shell = "",
                                            const std::vector<std::string>& add_groups = {},
                                            const std::vector<std::string>& remove_groups = {},
                                            const std::string& full_name = "");

    /// Grant sudo access to a user.
    Result<void, std::string> grant_sudo(const std::string& username);

    /// Revoke sudo access from a user.
    Result<void, std::string> revoke_sudo(const std::string& username);

    /// Check if user has sudo access.
    bool has_sudo(const std::string& username) const;

    /// Lock a user account (disable login).
    Result<void, std::string> lock_user(const std::string& username);

    /// Unlock a user account.
    Result<void, std::string> unlock_user(const std::string& username);

    /// Add an SSH public key for a user.
    Result<void, std::string> add_ssh_key(const std::string& username,
                                            const std::string& key);

    /// Remove an SSH key by fingerprint or index.
    Result<void, std::string> remove_ssh_key(const std::string& username,
                                               int index);

    /// List SSH keys for a user.
    Result<std::vector<SshKey>, std::string> list_ssh_keys(
        const std::string& username) const;

    /// Get login history for a user.
    Result<std::vector<LoginRecord>, std::string> login_history(
        const std::string& username,
        int max_entries = 20) const;

    /// Set password for a user.
    Result<void, std::string> set_password(const std::string& username,
                                             const std::string& password);

    /// Get password policy.
    PasswordPolicy get_password_policy() const;

    /// Set password policy.
    Result<void, std::string> set_password_policy(const PasswordPolicy& policy);

private:
    /// Run a shell command and capture output.
    Result<std::string, std::string> run_cmd(const std::string& cmd) const;

    /// Parse /etc/passwd for user entries.
    std::vector<UserAccount> parse_passwd(bool include_system) const;

    /// Get groups for a user.
    std::vector<std::string> get_groups(const std::string& username) const;

    /// Get SSH authorized_keys path for a user.
    std::string ssh_keys_path(const std::string& username) const;

    /// Get the home directory for a user.
    std::string home_dir(const std::string& username) const;

    /// Check if account is locked via passwd status.
    bool is_locked(const std::string& username) const;

    /// Get sudoers.d path for a user.
    std::string sudoers_path(const std::string& username) const;
};

} // namespace straylight
