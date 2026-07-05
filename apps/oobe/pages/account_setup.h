// apps/oobe/pages/account_setup.h
// OOBE account setup page
#pragma once

#include <string>

namespace straylight::oobe {

/// Account setup page — configure user identity and optionally add users.
class AccountSetupPage {
public:
    AccountSetupPage() = default;
    ~AccountSetupPage() = default;

    /// Render the account setup page.
    /// @return true if the user clicked "Next" (advance to next page).
    bool render();

    /// Validate a username against StrayLight rules.
    /// Rules: lowercase alphanumeric + underscore, 3-32 chars, not reserved.
    static bool validate_username(const std::string& username,
                                  std::string& error_out);

    /// Estimate password strength (0-4 score, zxcvbn-style).
    static int password_strength(const std::string& password);

private:
    char fullname_buf_[128] = {};
    char username_buf_[64]  = {};
    char password_buf_[128] = {};
    char confirm_buf_[128]  = {};
    std::string error_message_;
    bool add_user_ = false;
};

} // namespace straylight::oobe
