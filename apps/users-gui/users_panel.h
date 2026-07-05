// apps/users-gui/users_panel.h
// StrayLight User Management panel
#pragma once

#include <imgui.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
// >>> STRAYLIGHT-WIRE: users-gui os datapath <<<
#include <cmath>
#include <ctime>
#include <array>
#include <fstream>
#include <sstream>
#include <string>
#include <algorithm>
#include <pwd.h>
#include <grp.h>
#include <unistd.h>
#include <cstdlib>

namespace straylight::users {

struct UserInfo {
    char username[64];
    char display_name[64];
    char groups[128];
    char last_login[32];
    char shell[32];
    char home[128];
    char ssh_key[512];
    bool is_admin;
    bool locked;
};

struct GroupInfo {
    char name[64];
    int  gid;
    int  member_count;
};

struct UsersState {
    std::vector<UserInfo> user_list;
    std::vector<GroupInfo> group_list;
    int selected_user = 0;

    bool show_add_user = false;
    bool show_edit_user = false;
    bool show_add_group = false;

    char new_username[64] = {};
    char new_display[64] = {};
    char new_groups[128] = {};
    char new_shell[32] = {};
    char new_password[128] = {};
    bool new_is_admin = false;

    char new_group_name[64] = {};

    // >>> STRAYLIGHT-WIRE: users-gui os datapath <<<
    // OS-backed live state (no fabricated accounts). Reads the glibc NSS layer
    // (getpwent / getgrent / getgrouplist) plus authorized_keys, lastlog and
    // `passwd -S`. Mirrors the proven parsing in apps/settings/pages/users.cpp.
    bool        ok_ = false;
    std::string err_;
    double      last_refresh_ = -1.0e9;

    static bool is_admin_group(const std::string& g) {
        return g == "sudo" || g == "wheel" || g == "adm";
    }

    // First non-empty/non-comment line of ~user/.ssh/authorized_keys.
    static std::string read_ssh_key(const std::string& home) {
        if (home.empty()) return std::string();
        std::ifstream f(home + "/.ssh/authorized_keys");
        if (!f.is_open()) return std::string();
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;
            return line;
        }
        return std::string();
    }

    // Locked iff `passwd -S <user>` reports L (or LK). Returns false when the
    // status cannot be read (never fabricate a lock state).
    static bool read_locked(const std::string& user) {
        if (user.empty()) return false;
        std::string cmd = "passwd -S '" + user + "' 2>/dev/null";
        FILE* p = popen(cmd.c_str(), "r");
        if (!p) return false;
        char buf[256] = {};
        std::string out;
        while (fgets(buf, sizeof(buf), p)) out += buf;
        pclose(p);
        std::istringstream ss(out);
        std::string name, status;
        ss >> name >> status;
        return status == "L" || status == "LK";
    }

    // Last login from /var/log/lastlog: per-uid fixed-size record at uid*reclen.
    // ll_time==0 means "never". Returns empty when unreadable (no fabrication).
    static std::string read_last_login(unsigned int uid) {
        std::ifstream f("/var/log/lastlog", std::ios::binary);
        if (!f.is_open()) return std::string();
        // struct lastlog { int32 ll_time; char ll_line[32]; char ll_host[256]; }
        const long reclen = 4 + 32 + 256;
        f.seekg(static_cast<std::streamoff>(uid) * reclen, std::ios::beg);
        int32_t ll_time = 0;
        if (!f.read(reinterpret_cast<char*>(&ll_time), sizeof(ll_time))) return std::string();
        if (ll_time == 0) return std::string("Never");
        time_t t = static_cast<time_t>(ll_time);
        struct tm tmv{};
        if (!localtime_r(&t, &tmv)) return std::string();
        char out[32] = {};
        strftime(out, sizeof(out), "%Y-%m-%d %H:%M", &tmv);
        return std::string(out);
    }

    // Comma-separated list of supplementary + primary group names for a user.
    static std::string read_groups(const char* name, gid_t gid) {
        std::string csv;
        int ng = 0;
        getgrouplist(name, gid, nullptr, &ng);
        if (ng <= 0) ng = 64;
        std::vector<gid_t> gids(static_cast<size_t>(ng));
        if (getgrouplist(name, gid, gids.data(), &ng) < 0) {
            gids.resize(static_cast<size_t>(ng));
            getgrouplist(name, gid, gids.data(), &ng);
        }
        for (int i = 0; i < ng; ++i) {
            struct group grp_buf{};
            std::array<char, 8192> gbuf{};
            struct group* gr = nullptr;
            if (getgrgid_r(gids[static_cast<size_t>(i)], &grp_buf, gbuf.data(), gbuf.size(), &gr) == 0 && gr) {
                if (!csv.empty()) csv += ",";
                csv += gr->gr_name;
            }
        }
        return csv;
    }

    void refresh() {
        user_list.clear();
        group_list.clear();
        ok_ = false;
        err_.clear();

        // --- Users: getpwent (uid==0 or uid>=1000, skip nobody) -----------
        setpwent();
        for (struct passwd* pw = getpwent(); pw != nullptr; pw = getpwent()) {
            unsigned int uid = static_cast<unsigned int>(pw->pw_uid);
            if (!(uid == 0 || uid >= 1000)) continue;
            if (uid == 65534) continue; // nobody

            UserInfo u{};
            snprintf(u.username, sizeof(u.username), "%s", pw->pw_name ? pw->pw_name : "");

            // display_name = first GECOS field; fall back to username.
            std::string gecos = pw->pw_gecos ? pw->pw_gecos : "";
            auto comma = gecos.find(',');
            if (comma != std::string::npos) gecos = gecos.substr(0, comma);
            if (gecos.empty()) gecos = u.username;
            snprintf(u.display_name, sizeof(u.display_name), "%s", gecos.c_str());

            snprintf(u.home, sizeof(u.home), "%s", pw->pw_dir ? pw->pw_dir : "");
            snprintf(u.shell, sizeof(u.shell), "%s", pw->pw_shell ? pw->pw_shell : "");

            std::string groups = read_groups(pw->pw_name, pw->pw_gid);
            snprintf(u.groups, sizeof(u.groups), "%s", groups.c_str());

            std::string key = read_ssh_key(u.home);
            snprintf(u.ssh_key, sizeof(u.ssh_key), "%s", key.c_str());

            std::string ll = read_last_login(uid);
            snprintf(u.last_login, sizeof(u.last_login), "%s", ll.c_str());

            // is_admin = member of sudo/wheel/adm.
            u.is_admin = false;
            {
                std::istringstream gs(groups);
                std::string g;
                while (std::getline(gs, g, ',')) {
                    if (is_admin_group(g)) { u.is_admin = true; break; }
                }
            }

            u.locked = read_locked(u.username);

            user_list.push_back(u);
        }
        endpwent();

        // --- Groups: getgrent (name/gid/member_count) ---------------------
        setgrent();
        for (struct group* gr = getgrent(); gr != nullptr; gr = getgrent()) {
            GroupInfo g{};
            snprintf(g.name, sizeof(g.name), "%s", gr->gr_name ? gr->gr_name : "");
            g.gid = static_cast<int>(gr->gr_gid);
            int n = 0;
            if (gr->gr_mem) for (char** m = gr->gr_mem; *m != nullptr; ++m) ++n;
            g.member_count = n;
            group_list.push_back(g);
        }
        endgrent();

        if (user_list.empty()) {
            err_ = "no user accounts readable from NSS (getpwent returned nothing)";
            ok_ = false;
        } else {
            ok_ = true;
        }
    }

    void maybe_refresh() {
        double now = ImGui::GetTime();
        if (now - last_refresh_ >= 2.0) {
            last_refresh_ = now;
            refresh();
        }
    }

    void init() {
        refresh();
    }
};

inline void render_users_panel(UsersState& st) {
    if (st.user_list.empty()) st.init();
    // >>> STRAYLIGHT-WIRE: users-gui os datapath <<<
    st.maybe_refresh();
    if (!st.ok_) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.3f, 1.0f));
        ImGui::TextWrapped("user data unavailable: %s", st.err_.c_str());
        ImGui::PopStyleColor();
        if (ImGui::SmallButton("Retry")) st.refresh();
        ImGui::Separator();
    }

    const ImVec4 accent(0.0f, 1.0f, 0.67f, 1.0f);

    ImGui::PushStyleColor(ImGuiCol_Text, accent);
    ImGui::Text("USER MANAGEMENT");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();

    // Toolbar
    if (ImGui::Button("Add User", ImVec2(120, 30))) {
        st.show_add_user = true;
        memset(st.new_username, 0, sizeof(st.new_username));
        memset(st.new_display, 0, sizeof(st.new_display));
        memset(st.new_groups, 0, sizeof(st.new_groups));
        snprintf(st.new_shell, 32, "/bin/bash");
        st.new_is_admin = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Add Group", ImVec2(120, 30))) {
        st.show_add_group = true;
        memset(st.new_group_name, 0, sizeof(st.new_group_name));
    }
    ImGui::Spacing();

    float list_w = ImGui::GetContentRegionAvail().x * 0.4f;

    // User list
    ImGui::BeginChild("##user_list", ImVec2(list_w, ImGui::GetContentRegionAvail().y * 0.65f), true);
    ImGui::TextColored(accent, "Users (%zu)", st.user_list.size());
    ImGui::Separator();

    for (int i = 0; i < (int)st.user_list.size(); ++i) {
        auto& u = st.user_list[i];
        ImGui::PushID(i);

        bool selected = (i == st.selected_user);

        // Avatar placeholder (colored circle with initial)
        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        float rad = 14;
        ImVec2 center(pos.x + rad + 4, pos.y + rad + 4);
        ImU32 avatar_col = u.is_admin ? IM_COL32(0, 200, 100, 255) : IM_COL32(80, 80, 140, 255);
        if (u.locked) avatar_col = IM_COL32(120, 60, 60, 255);
        draw->AddCircleFilled(center, rad, avatar_col);
        char initial[2] = {u.username[0], 0};
        if (initial[0] >= 'a') initial[0] -= 32; // uppercase
        ImVec2 ts = ImGui::CalcTextSize(initial);
        draw->AddText(ImVec2(center.x - ts.x * 0.5f, center.y - ts.y * 0.5f),
                      IM_COL32(255, 255, 255, 255), initial);

        ImGui::Dummy(ImVec2(rad * 2 + 8, 0));
        ImGui::SameLine();

        char label[160];
        snprintf(label, sizeof(label), "%s (%s)%s", u.display_name, u.username,
                 u.locked ? " [LOCKED]" : "");
        if (ImGui::Selectable(label, selected, 0, ImVec2(0, 32))) {
            st.selected_user = i;
        }

        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // User detail panel
    ImGui::BeginChild("##user_detail", ImVec2(0, ImGui::GetContentRegionAvail().y * 0.65f), true);
    if (st.selected_user >= 0 && st.selected_user < (int)st.user_list.size()) {
        auto& u = st.user_list[st.selected_user];
        ImGui::TextColored(accent, "%s", u.display_name);
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("Username:");    ImGui::SameLine(130); ImGui::Text("%s", u.username);
        ImGui::Text("Home:");        ImGui::SameLine(130); ImGui::Text("%s", u.home);
        ImGui::Text("Shell:");       ImGui::SameLine(130); ImGui::Text("%s", u.shell);
        ImGui::Text("Groups:");      ImGui::SameLine(130); ImGui::TextWrapped("%s", u.groups);
        ImGui::Text("Last Login:");  ImGui::SameLine(130); ImGui::Text("%s", u.last_login);
        ImGui::Text("Admin:");       ImGui::SameLine(130); ImGui::Text("%s", u.is_admin ? "Yes" : "No");
        ImGui::Text("Status:");      ImGui::SameLine(130);
        if (u.locked) ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "Locked");
        else ImGui::TextColored(accent, "Active");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextColored(accent, "SSH Keys");
        ImGui::Spacing();
        if (strlen(u.ssh_key) > 0) {
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.05f, 0.05f, 0.08f, 1.0f));
            char key_buf[512];
            snprintf(key_buf, 512, "%s", u.ssh_key);
            ImGui::InputTextMultiline("##ssh", key_buf, 512, ImVec2(-1, 60), ImGuiInputTextFlags_ReadOnly);
            ImGui::PopStyleColor();
        } else {
            ImGui::TextDisabled("No SSH key configured");
        }

        ImGui::Spacing();
        if (ImGui::Button("Edit User", ImVec2(120, 28))) {
            st.show_edit_user = true;
            snprintf(st.new_username, 64, "%s", u.username);
            snprintf(st.new_display, 64, "%s", u.display_name);
            snprintf(st.new_groups, 128, "%s", u.groups);
            snprintf(st.new_shell, 32, "%s", u.shell);
            st.new_is_admin = u.is_admin;
        }
        ImGui::SameLine();
        if (u.locked) {
            if (ImGui::Button("Unlock", ImVec2(100, 28))) u.locked = false;
        } else {
            if (ImGui::Button("Lock", ImVec2(100, 28))) u.locked = true;
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.1f, 0.1f, 0.8f));
        if (ImGui::Button("Delete", ImVec2(100, 28))) {
            // delete user
        }
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();

    // Group management
    ImGui::BeginChild("##groups", ImVec2(-1, 0), true);
    ImGui::TextColored(accent, "Groups");
    ImGui::Separator();

    if (ImGui::BeginTable("##grp_table", 3,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Group Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("GID", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("Members", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableHeadersRow();

        for (auto& g : st.group_list) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("%s", g.name);
            ImGui::TableSetColumnIndex(1); ImGui::Text("%d", g.gid);
            ImGui::TableSetColumnIndex(2); ImGui::Text("%d", g.member_count);
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    // Add User Dialog
    if (st.show_add_user) {
        ImGui::OpenPopup("Add User");
        st.show_add_user = false;
    }
    if (ImGui::BeginPopupModal("Add User", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("Username", st.new_username, 64);
        ImGui::InputText("Display Name", st.new_display, 64);
        ImGui::InputText("Groups", st.new_groups, 128);
        ImGui::InputText("Shell", st.new_shell, 32);
        ImGui::InputText("Password", st.new_password, 128, ImGuiInputTextFlags_Password);
        ImGui::Checkbox("Administrator", &st.new_is_admin);
        ImGui::Spacing();
        if (ImGui::Button("Create", ImVec2(120, 30)) && strlen(st.new_username) > 0) {
            UserInfo nu{};
            snprintf(nu.username, 64, "%s", st.new_username);
            snprintf(nu.display_name, 64, "%s", st.new_display);
            snprintf(nu.groups, 128, "%s", st.new_groups);
            snprintf(nu.shell, 32, "%s", st.new_shell);
            snprintf(nu.last_login, 32, "Never");
            snprintf(nu.home, 128, "/home/%s", st.new_username);
            nu.is_admin = st.new_is_admin;
            nu.locked = false;
            st.user_list.push_back(nu);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 30))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // Edit User Dialog
    if (st.show_edit_user) {
        ImGui::OpenPopup("Edit User");
        st.show_edit_user = false;
    }
    if (ImGui::BeginPopupModal("Edit User", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("Display Name", st.new_display, 64);
        ImGui::InputText("Groups", st.new_groups, 128);
        ImGui::InputText("Shell", st.new_shell, 32);
        ImGui::Checkbox("Administrator", &st.new_is_admin);
        ImGui::Spacing();
        if (ImGui::Button("Save", ImVec2(120, 30))) {
            if (st.selected_user >= 0 && st.selected_user < (int)st.user_list.size()) {
                auto& u = st.user_list[st.selected_user];
                snprintf(u.display_name, 64, "%s", st.new_display);
                snprintf(u.groups, 128, "%s", st.new_groups);
                snprintf(u.shell, 32, "%s", st.new_shell);
                u.is_admin = st.new_is_admin;
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 30))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // Add Group Dialog
    if (st.show_add_group) {
        ImGui::OpenPopup("Add Group");
        st.show_add_group = false;
    }
    if (ImGui::BeginPopupModal("Add Group", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("Group Name", st.new_group_name, 64);
        ImGui::Spacing();
        if (ImGui::Button("Create", ImVec2(120, 30)) && strlen(st.new_group_name) > 0) {
            GroupInfo ng{};
            snprintf(ng.name, 64, "%s", st.new_group_name);
            ng.gid = 2000 + (int)st.group_list.size();
            ng.member_count = 0;
            st.group_list.push_back(ng);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 30))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

} // namespace straylight::users
