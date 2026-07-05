// apps/oobe/pages/account_setup.cpp
// Account setup page implementation
#include "account_setup.h"

#include <straylight/log.h>

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <set>
#include <string>

namespace straylight::oobe {

// Reserved usernames that cannot be used
static const std::set<std::string> kReservedNames = {
    "root", "daemon", "bin", "sys", "sync", "games", "man",
    "lp", "mail", "news", "uucp", "proxy", "www-data",
    "backup", "list", "irc", "gnats", "nobody", "systemd-network",
    "systemd-resolve", "messagebus", "sshd", "straylight"
};

bool AccountSetupPage::validate_username(const std::string& username,
                                         std::string& error_out) {
    if (username.length() < 3) {
        error_out = "Username must be at least 3 characters";
        return false;
    }
    if (username.length() > 32) {
        error_out = "Username must be at most 32 characters";
        return false;
    }

    for (char c : username) {
        if (!std::islower(c) && !std::isdigit(c) && c != '_') {
            error_out = "Username must contain only lowercase letters, "
                        "digits, and underscores";
            return false;
        }
    }

    if (kReservedNames.count(username)) {
        error_out = "Username '" + username + "' is reserved";
        return false;
    }

    error_out.clear();
    return true;
}

int AccountSetupPage::password_strength(const std::string& password) {
    if (password.empty()) return 0;

    int score = 0;

    // Length scoring
    if (password.length() >= 8)  score++;
    if (password.length() >= 12) score++;

    // Character class diversity
    bool has_lower = false, has_upper = false;
    bool has_digit = false, has_special = false;
    for (char c : password) {
        if (std::islower(c)) has_lower = true;
        else if (std::isupper(c)) has_upper = true;
        else if (std::isdigit(c)) has_digit = true;
        else has_special = true;
    }

    int classes = has_lower + has_upper + has_digit + has_special;
    if (classes >= 3) score++;
    if (classes >= 4) score++;

    return std::min(score, 4);
}

bool AccountSetupPage::render() {
    const ImGuiIO& io = ImGui::GetIO();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse;

    ImGui::Begin("##OobeAccount", nullptr, flags);

    ImGui::SetCursorPosY(60.0f);
    ImGui::SetCursorPosX(60.0f);
    ImGui::Text("Account Setup");
    ImGui::Separator();
    ImGui::Spacing();

    // Full name
    ImGui::SetCursorPosX(60.0f);
    ImGui::Text("Full Name");
    ImGui::SetCursorPosX(60.0f);
    ImGui::SetNextItemWidth(400.0f);
    ImGui::InputText("##fullname", fullname_buf_, sizeof(fullname_buf_));

    ImGui::Spacing();

    // Add additional user checkbox
    ImGui::SetCursorPosX(60.0f);
    ImGui::Checkbox("Add a new user account", &add_user_);

    if (add_user_) {
        ImGui::Spacing();

        ImGui::SetCursorPosX(60.0f);
        ImGui::Text("Username");
        ImGui::SetCursorPosX(60.0f);
        ImGui::SetNextItemWidth(400.0f);
        ImGui::InputText("##username", username_buf_, sizeof(username_buf_));

        ImGui::Spacing();

        ImGui::SetCursorPosX(60.0f);
        ImGui::Text("Password");
        ImGui::SetCursorPosX(60.0f);
        ImGui::SetNextItemWidth(400.0f);
        ImGui::InputText("##password", password_buf_, sizeof(password_buf_),
                         ImGuiInputTextFlags_Password);

        // Password strength indicator
        int strength = password_strength(std::string(password_buf_));
        ImGui::SetCursorPosX(60.0f);
        const char* labels[] = {"Very Weak", "Weak", "Fair", "Strong",
                                "Very Strong"};
        ImVec4 colors[] = {
            {1.0f, 0.2f, 0.2f, 1.0f},
            {1.0f, 0.5f, 0.2f, 1.0f},
            {1.0f, 0.8f, 0.2f, 1.0f},
            {0.4f, 0.8f, 0.2f, 1.0f},
            {0.2f, 0.9f, 0.4f, 1.0f},
        };
        if (strlen(password_buf_) > 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, colors[strength]);
            ImGui::Text("Strength: %s", labels[strength]);
            ImGui::PopStyleColor();
        }

        ImGui::Spacing();

        ImGui::SetCursorPosX(60.0f);
        ImGui::Text("Confirm Password");
        ImGui::SetCursorPosX(60.0f);
        ImGui::SetNextItemWidth(400.0f);
        ImGui::InputText("##confirm", confirm_buf_, sizeof(confirm_buf_),
                         ImGuiInputTextFlags_Password);
    }

    // Error message
    if (!error_message_.empty()) {
        ImGui::Spacing();
        ImGui::SetCursorPosX(60.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.3f, 0.3f, 1));
        ImGui::TextWrapped("%s", error_message_.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    ImGui::Spacing();

    // Next button
    ImGui::SetCursorPosX(60.0f);
    bool advance = false;
    if (ImGui::Button("Next", ImVec2(120, 40))) {
        error_message_.clear();

        if (add_user_) {
            std::string username_str(username_buf_);
            std::string err;
            if (!validate_username(username_str, err)) {
                error_message_ = err;
            } else if (strlen(password_buf_) == 0) {
                error_message_ = "Password cannot be empty";
            } else if (strcmp(password_buf_, confirm_buf_) != 0) {
                error_message_ = "Passwords do not match";
            } else {
                // TODO: Actually create user via useradd + chpasswd
                SL_INFO("Would create user: {}", username_str);
                advance = true;
            }
        } else {
            // Just updating full name if changed
            if (strlen(fullname_buf_) > 0) {
                SL_INFO("Would update full name via chfn: {}",
                        fullname_buf_);
            }
            advance = true;
        }
    }

    ImGui::End();
    return advance;
}

} // namespace straylight::oobe
