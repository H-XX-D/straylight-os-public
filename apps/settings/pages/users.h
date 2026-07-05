// apps/settings/pages/users.h
// User account management — reading /etc/passwd, adding/removing users
#pragma once

#include "../settings_page.h"

#include <straylight/result.h>

#include <cstdint>
#include <string>
#include <vector>

#include <sys/types.h>

namespace straylight::settings {

/// Information for a single local user account.
struct UserInfo {
    uid_t       uid        = 0;
    gid_t       gid        = 0;
    std::string username;
    std::string gecos;      // Full name / GECOS field
    std::string home;
    std::string shell;
    bool        is_admin   = false; // Member of sudo or wheel group
    bool        is_system  = false; // uid < 1000 or uid == 65534
};

/// User management page — lists local accounts and allows adding/removing them.
/// Account creation uses the system useradd utility (requires root/sudo).
/// Account deletion uses userdel -r (removes home directory).
class UsersPage : public SettingsPage {
public:
    UsersPage()  = default;
    ~UsersPage() = default;

    [[nodiscard]] const char* label() const override { return "Users"; }

    /// Read /etc/passwd and check sudo/wheel group membership.
    void load() override;

    /// Render the user management panel.
    void render() override;

private:
    /// Parse /etc/passwd into users_.
    void parse_passwd();

    /// Check if uid is in the sudo or wheel group via /etc/group.
    bool is_in_admin_group(uid_t uid) const;

    /// Add a new user account via useradd.
    Result<void, std::string> add_user(const std::string& name, bool admin,
                                       const std::string& full_name);

    /// Remove a user account and their home directory via userdel -r.
    Result<void, std::string> remove_user(const std::string& name);

    /// Change a user's password interactively via passwd.
    Result<void, std::string> change_password(const std::string& name,
                                               const std::string& new_pass);

    /// Toggle admin (sudo) group membership.
    Result<void, std::string> set_admin(const std::string& name, bool admin);

    std::vector<UserInfo> users_;
    bool show_system_users_ = false;  // Toggle to show uid < 1000

    // Add user form state
    char new_username_[64]  = {};
    char new_fullname_[128] = {};
    bool new_is_admin_      = false;
    bool show_add_form_     = false;

    // Password change form state
    int  passwd_user_idx_     = -1;  // Index into users_ being changed
    char new_password_[128]   = {};
    char confirm_password_[128] = {};
    bool show_passwd_form_    = false;

    // Pending delete confirmation
    int  delete_confirm_idx_  = -1;

    std::string status_msg_;
};

} // namespace straylight::settings
