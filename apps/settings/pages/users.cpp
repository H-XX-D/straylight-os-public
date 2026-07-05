// apps/settings/pages/users.cpp
// User account management — parsing /etc/passwd and invoking useradd/userdel
#include "users.h"

#include <imgui.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <grp.h>
#include <pwd.h>
#include <sstream>
#include <string>
#include <unistd.h>

namespace straylight::settings {

// ---------------------------------------------------------------------------
// parse_passwd()
// ---------------------------------------------------------------------------

void UsersPage::parse_passwd() {
    users_.clear();

    std::ifstream f("/etc/passwd");
    if (!f.is_open()) return;

    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;

        // Format: username:x:uid:gid:gecos:home:shell
        std::istringstream ss(line);
        std::string username, x, uid_s, gid_s, gecos, home, shell;

        if (!std::getline(ss, username, ':')) continue;
        if (!std::getline(ss, x, ':'))        continue;
        if (!std::getline(ss, uid_s, ':'))     continue;
        if (!std::getline(ss, gid_s, ':'))     continue;
        if (!std::getline(ss, gecos, ':'))     continue;
        if (!std::getline(ss, home, ':'))      continue;
        std::getline(ss, shell);

        if (uid_s.empty() || gid_s.empty()) continue;

        uid_t uid = 0;
        gid_t gid = 0;
        try {
            uid = static_cast<uid_t>(std::stoul(uid_s));
            gid = static_cast<gid_t>(std::stoul(gid_s));
        } catch (...) {
            continue;
        }

        UserInfo u;
        u.uid       = uid;
        u.gid       = gid;
        u.username  = username;
        u.gecos     = gecos;
        u.home      = home;
        u.shell     = shell;
        u.is_system = (uid < 1000 || uid == 65534);
        u.is_admin  = is_in_admin_group(uid);

        users_.push_back(std::move(u));
    }
}

// ---------------------------------------------------------------------------
// is_in_admin_group()
// ---------------------------------------------------------------------------

bool UsersPage::is_in_admin_group(uid_t uid) const {
    // Get the username for this uid
    struct passwd pw_buf{};
    std::array<char, 4096> buf{};
    struct passwd* pw = nullptr;
    if (getpwuid_r(uid, &pw_buf, buf.data(), buf.size(), &pw) != 0 || !pw) {
        return false;
    }
    std::string username = pw->pw_name;

    // Check /etc/group for sudo, wheel, and adm groups
    static const char* kAdminGroups[] = {"sudo", "wheel", "adm", nullptr};

    std::ifstream f("/etc/group");
    if (!f.is_open()) return false;

    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;

        // Format: group_name:x:gid:member1,member2,...
        std::istringstream ss(line);
        std::string grp_name, x, gid_s, members;

        if (!std::getline(ss, grp_name, ':')) continue;
        if (!std::getline(ss, x, ':'))        continue;
        if (!std::getline(ss, gid_s, ':'))    continue;
        std::getline(ss, members);

        // Is this an admin group?
        bool is_admin_grp = false;
        for (int i = 0; kAdminGroups[i]; ++i) {
            if (grp_name == kAdminGroups[i]) {
                is_admin_grp = true;
                break;
            }
        }
        if (!is_admin_grp) continue;

        // Check if username is in members
        std::istringstream mem_ss(members);
        std::string member;
        while (std::getline(mem_ss, member, ',')) {
            if (member == username) return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// load()
// ---------------------------------------------------------------------------

void UsersPage::load() {
    parse_passwd();
    status_msg_.clear();
}

// ---------------------------------------------------------------------------
// add_user()
// ---------------------------------------------------------------------------

Result<void, std::string> UsersPage::add_user(const std::string& name,
                                               bool admin,
                                               const std::string& full_name) {
    if (name.empty()) {
        return Result<void, std::string>::error("Username cannot be empty");
    }

    // Validate: only alphanumeric + underscore + hyphen, starting with letter
    for (char c : name) {
        if (!isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') {
            return Result<void, std::string>::error(
                "Invalid username: only alphanumeric, '_', '-' allowed");
        }
    }

    std::string cmd = "useradd -m -s /bin/bash";
    if (!full_name.empty()) {
        cmd += " -c '";
        cmd += full_name;
        cmd += "'";
    }
    if (admin) {
        cmd += " -G sudo,adm";
    }
    cmd += " '";
    cmd += name;
    cmd += "' 2>/dev/null";

    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        return Result<void, std::string>::error(
            std::string("useradd failed (exit ") + std::to_string(rc) +
            "). Are you running as root?");
    }
    return Result<void, std::string>::ok();
}

// ---------------------------------------------------------------------------
// remove_user()
// ---------------------------------------------------------------------------

Result<void, std::string> UsersPage::remove_user(const std::string& name) {
    if (name.empty()) {
        return Result<void, std::string>::error("Username is empty");
    }

    // Safety: never remove root
    if (name == "root") {
        return Result<void, std::string>::error("Cannot remove root account");
    }

    // Also never remove the currently running user
    uid_t my_uid = getuid();
    struct passwd pw_buf{};
    std::array<char, 4096> buf{};
    struct passwd* pw = nullptr;
    if (getpwuid_r(my_uid, &pw_buf, buf.data(), buf.size(), &pw) == 0 && pw) {
        if (name == pw->pw_name) {
            return Result<void, std::string>::error(
                "Cannot remove the currently logged-in user");
        }
    }

    std::string cmd = "userdel -r '";
    cmd += name;
    cmd += "' 2>/dev/null";

    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        return Result<void, std::string>::error(
            std::string("userdel failed (exit ") + std::to_string(rc) +
            "). Are you running as root?");
    }
    return Result<void, std::string>::ok();
}

// ---------------------------------------------------------------------------
// change_password()
// ---------------------------------------------------------------------------

Result<void, std::string> UsersPage::change_password(const std::string& name,
                                                       const std::string& new_pass) {
    if (new_pass.empty()) {
        return Result<void, std::string>::error("Password cannot be empty");
    }

    // Use chpasswd to set the password non-interactively
    std::string input = name + ":" + new_pass + "\n";
    std::string cmd = "echo '" + input + "' | chpasswd 2>/dev/null";
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        return Result<void, std::string>::error(
            "chpasswd failed. Are you running as root?");
    }
    return Result<void, std::string>::ok();
}

// ---------------------------------------------------------------------------
// set_admin()
// ---------------------------------------------------------------------------

Result<void, std::string> UsersPage::set_admin(const std::string& name, bool admin) {
    std::string cmd;
    if (admin) {
        cmd = "usermod -aG sudo,adm '" + name + "' 2>/dev/null";
    } else {
        cmd = "gpasswd -d '" + name + "' sudo 2>/dev/null; "
              "gpasswd -d '" + name + "' adm 2>/dev/null";
    }
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        return Result<void, std::string>::error(
            "usermod failed. Are you running as root?");
    }
    return Result<void, std::string>::ok();
}

// ---------------------------------------------------------------------------
// render()
// ---------------------------------------------------------------------------

void UsersPage::render() {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.67f, 1.0f));
    ImGui::Text("Users");
    ImGui::PopStyleColor();
    ImGui::Spacing();

    // Show system users toggle
    ImGui::Checkbox("Show system accounts (uid < 1000)", &show_system_users_);

    ImGui::SameLine(ImGui::GetWindowWidth() - 200.0f);
    if (ImGui::Button("Add User...")) {
        show_add_form_  = !show_add_form_;
        memset(new_username_, 0, sizeof(new_username_));
        memset(new_fullname_, 0, sizeof(new_fullname_));
        new_is_admin_  = false;
    }

    ImGui::Spacing();

    // --- Add User Form --------------------------------------------------
    if (show_add_form_) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.15f, 0.15f, 0.2f, 1.0f));
        if (ImGui::BeginChild("##addform", ImVec2(0, 120), true)) {
            ImGui::Text("New User Account");
            ImGui::Separator();

            ImGui::SetNextItemWidth(200.0f);
            ImGui::InputText("Username##new", new_username_, sizeof(new_username_));

            ImGui::SameLine();
            ImGui::SetNextItemWidth(250.0f);
            ImGui::InputText("Full Name##new", new_fullname_, sizeof(new_fullname_));

            ImGui::Checkbox("Administrator (sudo)##new", &new_is_admin_);

            ImGui::SameLine(400.0f);
            if (ImGui::Button("Create Account", ImVec2(140, 0))) {
                auto r = add_user(new_username_, new_is_admin_, new_fullname_);
                if (r.has_value()) {
                    status_msg_ = std::string("Created user: ") + new_username_;
                    show_add_form_ = false;
                    load();
                } else {
                    status_msg_ = r.error();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(80, 0))) {
                show_add_form_ = false;
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    // --- Password Change Form -------------------------------------------
    if (show_passwd_form_ && passwd_user_idx_ >= 0 &&
        passwd_user_idx_ < static_cast<int>(users_.size())) {
        const auto& u = users_[static_cast<size_t>(passwd_user_idx_)];

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.15f, 0.15f, 0.2f, 1.0f));
        if (ImGui::BeginChild("##passwdform", ImVec2(0, 110), true)) {
            ImGui::Text("Change password for: %s", u.username.c_str());
            ImGui::Separator();

            ImGui::SetNextItemWidth(220.0f);
            ImGui::InputText("New Password##pw",
                             new_password_, sizeof(new_password_),
                             ImGuiInputTextFlags_Password);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(220.0f);
            ImGui::InputText("Confirm##pwc",
                             confirm_password_, sizeof(confirm_password_),
                             ImGuiInputTextFlags_Password);

            if (ImGui::Button("Apply Password", ImVec2(130, 0))) {
                if (strcmp(new_password_, confirm_password_) != 0) {
                    status_msg_ = "Passwords do not match";
                } else {
                    auto r = change_password(u.username, new_password_);
                    if (r.has_value()) {
                        status_msg_ = "Password changed for: " + u.username;
                        show_passwd_form_ = false;
                    } else {
                        status_msg_ = r.error();
                    }
                    memset(new_password_,    0, sizeof(new_password_));
                    memset(confirm_password_, 0, sizeof(confirm_password_));
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel##pw", ImVec2(80, 0))) {
                show_passwd_form_ = false;
                memset(new_password_,    0, sizeof(new_password_));
                memset(confirm_password_, 0, sizeof(confirm_password_));
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    // --- User Table -----------------------------------------------------
    ImGui::Separator();

    constexpr ImGuiTableFlags table_flags =
        ImGuiTableFlags_Borders |
        ImGuiTableFlags_RowBg   |
        ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_Resizable;

    float table_h = ImGui::GetContentRegionAvail().y - 40.0f;
    if (ImGui::BeginTable("##usertable", 6, table_flags, ImVec2(0, table_h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Username", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn("Full Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("UID",       ImGuiTableColumnFlags_WidthFixed, 55.0f);
        ImGui::TableSetupColumn("Home",      ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Admin",     ImGuiTableColumnFlags_WidthFixed, 55.0f);
        ImGui::TableSetupColumn("Actions",   ImGuiTableColumnFlags_WidthFixed, 180.0f);
        ImGui::TableHeadersRow();

        for (int i = 0; i < static_cast<int>(users_.size()); ++i) {
            auto& u = users_[static_cast<size_t>(i)];
            if (u.is_system && !show_system_users_) continue;

            ImGui::TableNextRow();
            ImGui::PushID(i);

            // Username
            ImGui::TableSetColumnIndex(0);
            if (u.is_system) {
                ImGui::TextDisabled("%s", u.username.c_str());
            } else {
                ImGui::TextUnformatted(u.username.c_str());
            }

            // Full name (GECOS)
            ImGui::TableSetColumnIndex(1);
            // GECOS may have multiple comma-separated fields; use first
            std::string display_name = u.gecos;
            auto comma = display_name.find(',');
            if (comma != std::string::npos) display_name = display_name.substr(0, comma);
            ImGui::TextUnformatted(display_name.c_str());

            // UID
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%u", static_cast<unsigned int>(u.uid));

            // Home
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(u.home.c_str());

            // Admin
            ImGui::TableSetColumnIndex(4);
            if (!u.is_system) {
                bool admin = u.is_admin;
                if (ImGui::Checkbox("##admin", &admin)) {
                    auto r = set_admin(u.username, admin);
                    if (r.has_value()) {
                        u.is_admin  = admin;
                        status_msg_ = std::string(admin ? "Granted" : "Revoked") +
                                      " admin for: " + u.username;
                    } else {
                        status_msg_ = r.error();
                    }
                }
            }

            // Actions (only for non-system users)
            ImGui::TableSetColumnIndex(5);
            if (!u.is_system) {
                if (ImGui::SmallButton("Password")) {
                    passwd_user_idx_ = i;
                    show_passwd_form_ = true;
                    show_add_form_   = false;
                    memset(new_password_,    0, sizeof(new_password_));
                    memset(confirm_password_, 0, sizeof(confirm_password_));
                }
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button,
                                      ImVec4(0.6f, 0.1f, 0.1f, 1.0f));
                if (ImGui::SmallButton("Delete")) {
                    delete_confirm_idx_ = i;
                    ImGui::OpenPopup("ConfirmDelete");
                }
                ImGui::PopStyleColor();
            }

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // --- Delete Confirmation Popup -------------------------------------
    if (delete_confirm_idx_ >= 0 &&
        delete_confirm_idx_ < static_cast<int>(users_.size())) {
        const auto& du = users_[static_cast<size_t>(delete_confirm_idx_)];

        if (ImGui::BeginPopupModal("ConfirmDelete", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Delete user '%s' and their home directory?",
                        du.username.c_str());
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                               "This action cannot be undone!");
            ImGui::Separator();

            if (ImGui::Button("Delete", ImVec2(120, 0))) {
                auto r = remove_user(du.username);
                if (r.has_value()) {
                    status_msg_ = "Deleted user: " + du.username;
                    load();
                } else {
                    status_msg_ = r.error();
                }
                delete_confirm_idx_ = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                delete_confirm_idx_ = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    // --- Status Bar ----------------------------------------------------
    ImGui::Spacing();
    ImGui::Separator();
    if (!status_msg_.empty()) {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.6f, 1.0f),
                           "%s", status_msg_.c_str());
    }
}

} // namespace straylight::settings
