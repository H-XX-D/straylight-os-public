// apps/policy-gui/policy_panel.h
// StrayLight System Policy panel
#pragma once

#include <imgui.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <cctype>
#include <cstdlib>
#include <unistd.h>

namespace straylight::policy {

struct ComplianceCheck {
    char description[128];
    bool passed;
    char detail[128];
};

struct PolicyRule {
    char name[64];
    char value[128];
    bool enforced;
};

struct PolicyState {
    // [wired:os] policy compliance read from native OS sources
    int  current_role = 1;
    int  preview_role = -1;
    std::vector<ComplianceCheck> checks;
    std::vector<PolicyRule> custom_rules;

    bool show_custom_editor = false;
    char new_rule_name[64] = {};
    char new_rule_value[128] = {};

    // Live OS source link (no daemon; no fabricated data).
    bool        ok_ = false;
    std::string err_;
    double      last_refresh_ = -1.0e9;

    static constexpr const char* roles[] = {
        "Workstation", "Server", "Development", "Kiosk", "Minimal", "Custom"
    };
    static constexpr int num_roles = 6;

    static constexpr const char* role_descriptions[] = {
        "General purpose desktop with full GUI, multimedia, and productivity tools",
        "Headless server with hardened security, no GUI, minimal attack surface",
        "Development workstation with compilers, debuggers, containers, and dev tools",
        "Locked-down single-application kiosk mode with restricted access",
        "Bare minimum system with essential services only",
        "User-defined policy with manual rule configuration"
    };

    // --- small OS read helpers ---------------------------------------------
    static std::string read_file(const char* path) {
        std::ifstream f(path);
        if (!f.is_open()) return std::string();
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    static std::string run_cmd(const char* cmd) {
        std::string out;
        FILE* p = ::popen(cmd, "r");
        if (!p) return out;
        char buf[256];
        while (fgets(buf, sizeof(buf), p)) out += buf;
        ::pclose(p);
        // trim trailing whitespace/newline
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
            out.pop_back();
        return out;
    }

    // Count TCP/TCP6 sockets in LISTEN state (st field == 0x0A) from /proc/net.
    // Mirrors the /proc/net parsing pattern used in apps/system_monitor.
    static int count_listen_sockets() {
        int n = 0;
        const char* files[] = { "/proc/net/tcp", "/proc/net/tcp6" };
        for (const char* path : files) {
            std::ifstream f(path);
            if (!f.is_open()) continue;
            std::string line;
            std::getline(f, line); // header
            while (std::getline(f, line)) {
                std::istringstream ls(line);
                std::string sl, local, rem, st;
                ls >> sl >> local >> rem >> st;
                if (st == "0A") ++n;
            }
        }
        return n;
    }

    static void put(ComplianceCheck& c, const char* desc, bool passed, const std::string& detail) {
        std::snprintf(c.description, sizeof(c.description), "%s", desc);
        c.passed = passed;
        std::snprintf(c.detail, sizeof(c.detail), "%s", detail.c_str());
    }

    void ensure_open() {
        // OS source is always reachable on this host (procfs/sysfs/CLI).
        ok_ = true;
        err_.clear();
    }

    void refresh() {
        ensure_open();
        if (!ok_) return;

        std::vector<ComplianceCheck> out;
        ComplianceCheck c{};

        // (1) SSH root login disabled
        {
            std::string s = run_cmd(
                "grep -rhi '^[[:space:]]*PermitRootLogin' "
                "/etc/ssh/sshd_config /etc/ssh/sshd_config.d/* 2>/dev/null");
            std::string low = s;
            for (auto& ch : low) ch = (char)tolower((unsigned char)ch);
            bool disabled = (low.find("permitrootlogin no") != std::string::npos ||
                             low.find("permitrootlogin prohibit-password") != std::string::npos)
                            && low.find("permitrootlogin yes") == std::string::npos;
            std::string detail = s.empty() ? "PermitRootLogin not set (default)"
                                           : s.substr(0, s.find('\n'));
            put(c, "SSH root login disabled", disabled, detail);
            out.push_back(c);
        }

        // (2) Password complexity enforced (pwquality minlen + pam stack)
        {
            std::string pwq = read_file("/etc/security/pwquality.conf");
            std::string pam = read_file("/etc/pam.d/common-password");
            int minlen = -1;
            {
                std::istringstream ss(pwq);
                std::string line;
                while (std::getline(ss, line)) {
                    std::string t = line;
                    size_t h = t.find('#');
                    if (h != std::string::npos) t = t.substr(0, h);
                    size_t pos = t.find("minlen");
                    if (pos != std::string::npos) {
                        size_t eq = t.find('=', pos);
                        if (eq != std::string::npos) {
                            try { minlen = std::stoi(t.substr(eq + 1)); } catch (...) {}
                        }
                    }
                }
            }
            bool has_pwquality = pam.find("pam_pwquality") != std::string::npos;
            bool enforced = has_pwquality && minlen >= 8;
            std::string detail;
            if (enforced) {
                detail = "pam_pwquality minlen=" + std::to_string(minlen);
            } else if (!has_pwquality) {
                detail = "no pam_pwquality (pam_unix only)";
            } else {
                detail = (minlen < 0) ? "pam_pwquality but minlen unset"
                                      : "minlen too low (" + std::to_string(minlen) + ")";
            }
            put(c, "Password complexity enforced", enforced, detail);
            out.push_back(c);
        }

        // (3) Audit logging active
        {
            std::string s = run_cmd("systemctl is-active auditd 2>/dev/null");
            bool active = (s == "active");
            put(c, "Audit logging active", active, s.empty() ? "auditd not present" : s);
            out.push_back(c);
        }

        // (4) Firewall enabled
        {
            bool have_ufw  = !run_cmd("command -v ufw 2>/dev/null").empty();
            bool have_nft  = !run_cmd("command -v nft 2>/dev/null").empty();
            bool have_ipt  = !run_cmd("command -v iptables 2>/dev/null").empty();
            bool enabled = false;
            std::string detail;
            if (have_ufw) {
                std::string st = run_cmd("ufw status 2>/dev/null | head -1");
                enabled = st.find("active") != std::string::npos;
                detail = st.empty() ? "ufw present" : st;
            } else if (have_nft) {
                std::string rs = run_cmd("nft list ruleset 2>/dev/null | head -1");
                enabled = !rs.empty();
                detail = enabled ? "nft ruleset present" : "nft present, empty ruleset";
            } else if (have_ipt) {
                std::string rs = run_cmd("iptables -S 2>/dev/null");
                enabled = rs.find("-A ") != std::string::npos;
                detail = enabled ? "iptables rules present" : "iptables present, no rules";
            } else {
                detail = "no firewall binaries (ufw/nft/iptables)";
            }
            put(c, "Firewall enabled", enabled, detail);
            out.push_back(c);
        }

        // (5) Disk encryption enabled (LUKS / crypt device-mapper targets)
        {
            std::string lb = run_cmd(
                "lsblk -o NAME,TYPE,FSTYPE 2>/dev/null | grep -i crypt");
            std::string mapper = run_cmd(
                "ls /dev/mapper 2>/dev/null | grep -v '^control$'");
            bool encrypted = !lb.empty() || !mapper.empty();
            std::string detail;
            if (encrypted) {
                detail = mapper.empty() ? "crypt block device present"
                                        : "dm-crypt: " + mapper.substr(0, mapper.find('\n'));
            } else {
                detail = "no LUKS/crypt mappings";
            }
            put(c, "Disk encryption enabled", encrypted, detail);
            out.push_back(c);
        }

        // (6) SELinux/AppArmor active
        {
            bool selinux_enforce = false;
            std::string se = read_file("/sys/fs/selinux/enforce");
            if (!se.empty() && se[0] == '1') selinux_enforce = true;

            bool apparmor_dir = (::access("/sys/kernel/security/apparmor", F_OK) == 0);
            int aa_profiles = 0;
            {
                std::ifstream f("/sys/kernel/security/apparmor/profiles");
                std::string line;
                while (std::getline(f, line)) if (!line.empty()) ++aa_profiles;
            }
            bool active = selinux_enforce || (apparmor_dir && aa_profiles > 0);
            std::string detail;
            if (selinux_enforce) {
                detail = "SELinux enforcing";
            } else if (apparmor_dir) {
                detail = "AppArmor profiles loaded: " + std::to_string(aa_profiles);
            } else {
                detail = "no SELinux/AppArmor LSM";
            }
            put(c, "SELinux/AppArmor active", active, detail);
            out.push_back(c);
        }

        // (7) Automatic security updates
        {
            std::string cfg = read_file("/etc/apt/apt.conf.d/20auto-upgrades");
            bool enabled = false;
            if (!cfg.empty()) {
                // look for Unattended-Upgrade "1"
                std::string low = cfg;
                for (auto& ch : low) ch = (char)tolower((unsigned char)ch);
                enabled = low.find("unattended-upgrade\" \"1\"") != std::string::npos ||
                          low.find("unattended-upgrade\"  \"1\"") != std::string::npos ||
                          (low.find("unattended-upgrade") != std::string::npos &&
                           low.find("\"1\"") != std::string::npos);
            }
            std::string detail = cfg.empty() ? "20auto-upgrades absent"
                                             : (enabled ? "unattended-upgrades enabled"
                                                        : "unattended-upgrades disabled");
            put(c, "Automatic security updates", enabled, detail);
            out.push_back(c);
        }

        // (8) USB mass storage restricted
        {
            std::string usbguard = run_cmd("systemctl is-active usbguard 2>/dev/null");
            bool guard_active = (usbguard == "active");
            bool storage_loaded =
                !run_cmd("lsmod 2>/dev/null | grep '^usb_storage'").empty();
            bool restricted = guard_active || !storage_loaded;
            std::string detail;
            if (guard_active) {
                detail = "usbguard active";
            } else if (storage_loaded) {
                detail = "usb_storage loaded, usbguard inactive (allowed)";
            } else {
                detail = "usb_storage not loaded";
            }
            put(c, "USB mass storage restricted", restricted, detail);
            out.push_back(c);
        }

        // (9) Core dump disabled
        {
            std::string sd = read_file("/proc/sys/fs/suid_dumpable");
            std::string pat = run_cmd("sysctl -n kernel.core_pattern 2>/dev/null");
            bool suid_off = (!sd.empty() && sd[0] == '0');
            // core_pattern piped to a handler or empty also counts as restricted,
            // but suid_dumpable=0 is the primary signal per the spec.
            bool disabled = suid_off;
            std::string detail = "suid_dumpable=" +
                (sd.empty() ? std::string("?") : std::string(1, sd[0]));
            if (!pat.empty()) detail += ", core_pattern=" + pat.substr(0, 32);
            put(c, "Core dump disabled", disabled, detail);
            out.push_back(c);
        }

        // (10) Network services minimized (LISTEN sockets from /proc/net)
        {
            int listening = count_listen_sockets();
            bool minimized = (listening <= 8);
            std::string detail = std::to_string(listening) + " listening TCP sockets";
            put(c, "Network services minimized", minimized, detail);
            out.push_back(c);
        }

        checks.swap(out);
    }

    void maybe_refresh() {
        double now = ImGui::GetTime();
        if (now - last_refresh_ >= 2.0) {
            last_refresh_ = now;
            refresh();
        }
    }

    // Kept so the user-driven "Apply Role" / "Apply All Rules" buttons still
    // re-read live state. No fabrication; just a forced refresh of real data.
    void update_checks() {
        refresh();
    }

    void init() {
        refresh();

        custom_rules.push_back({"ssh.permit_root", "no", true});
        custom_rules.push_back({"firewall.default_policy", "deny", true});
        custom_rules.push_back({"password.min_length", "12", true});
        custom_rules.push_back({"session.idle_timeout", "900", true});
        custom_rules.push_back({"audit.enabled", "true", true});
        custom_rules.push_back({"update.auto_security", "true", true});
        custom_rules.push_back({"usb.mass_storage", "allow", false});
    }
};

inline void render_policy_panel(PolicyState& st) {

    st.maybe_refresh();
    if (!st.ok_) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
        ImGui::TextWrapped("policy data source unavailable: %s", st.err_.c_str());
        ImGui::PopStyleColor();
        if (ImGui::SmallButton("Retry")) st.refresh();
        ImGui::Separator();
    }
    // [wired:os] one-time seed of custom_rules + first real read;
    // live compliance data is refreshed by st.maybe_refresh() above.
    static bool s_inited = false;
    if (!s_inited) { st.init(); s_inited = true; }

    const ImVec4 accent(0.0f, 1.0f, 0.67f, 1.0f);

    ImGui::PushStyleColor(ImGuiCol_Text, accent);
    ImGui::Text("SYSTEM POLICY");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();

    // Current role display
    ImGui::BeginChild("##role_display", ImVec2(-1, 80), true);
    ImGui::Text("Current Role:");
    ImGui::SameLine(120);

    // Large role label
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 label_pos = ImGui::GetCursorScreenPos();
    ImVec2 label_size(ImGui::CalcTextSize(PolicyState::roles[st.current_role]).x * 2.0f + 20, 40);

    draw->AddRectFilled(label_pos, ImVec2(label_pos.x + label_size.x, label_pos.y + label_size.y),
                        IM_COL32(0, 80, 55, 200), 6.0f);
    draw->AddRect(label_pos, ImVec2(label_pos.x + label_size.x, label_pos.y + label_size.y),
                  IM_COL32(0, 255, 136, 255), 6.0f, 0, 2.0f);

    ImGui::SetWindowFontScale(2.0f);
    draw->AddText(ImVec2(label_pos.x + 10, label_pos.y + 6),
                  IM_COL32(0, 255, 136, 255), PolicyState::roles[st.current_role]);
    ImGui::SetWindowFontScale(1.0f);

    ImGui::Dummy(ImVec2(0, 40));
    ImGui::TextDisabled("%s", PolicyState::role_descriptions[st.current_role]);
    ImGui::EndChild();
    ImGui::Spacing();

    float left_w = ImGui::GetContentRegionAvail().x * 0.45f;

    // Role selector with preview
    ImGui::BeginChild("##role_select", ImVec2(left_w, ImGui::GetContentRegionAvail().y * 0.5f), true);
    ImGui::TextColored(accent, "Role Selector");
    ImGui::Separator();
    ImGui::Spacing();

    for (int i = 0; i < PolicyState::num_roles; ++i) {
        ImGui::PushID(i);
        bool is_current = (i == st.current_role);

        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImU32 bg = is_current ? IM_COL32(0, 60, 40, 200) : IM_COL32(25, 25, 40, 200);
        float role_w = ImGui::GetContentRegionAvail().x;

        if (ImGui::Selectable("##role", is_current || st.preview_role == i, 0, ImVec2(0, 50))) {
            st.preview_role = i;
        }

        // Overlay content
        ImDrawList* rdraw = ImGui::GetWindowDrawList();
        rdraw->AddRectFilled(pos, ImVec2(pos.x + role_w, pos.y + 50), bg, 4.0f);
        rdraw->AddText(ImVec2(pos.x + 8, pos.y + 4), IM_COL32(220, 220, 220, 255),
                       PolicyState::roles[i]);
        rdraw->AddText(ImVec2(pos.x + 8, pos.y + 22), IM_COL32(140, 140, 140, 200),
                       PolicyState::role_descriptions[i]);

        if (is_current) {
            rdraw->AddText(ImVec2(pos.x + ImGui::GetContentRegionAvail().x - 70, pos.y + 4),
                           IM_COL32(0, 255, 136, 255), "ACTIVE");
        }

        ImGui::PopID();
    }

    ImGui::Spacing();
    if (st.preview_role >= 0 && st.preview_role != st.current_role) {
        if (ImGui::Button("Apply Role", ImVec2(-1, 30))) {
            st.current_role = st.preview_role;
            st.update_checks();
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Compliance checklist
    ImGui::BeginChild("##compliance", ImVec2(0, ImGui::GetContentRegionAvail().y * 0.5f), true);
    ImGui::TextColored(accent, "Compliance Checklist");
    ImGui::Separator();
    ImGui::Spacing();

    int passed = 0, total = (int)st.checks.size();
    for (auto& c : st.checks) if (c.passed) passed++;
    ImGui::Text("Score: %d / %d", passed, total);

    // Progress
    ImGui::ProgressBar((float)passed / std::max(total, 1), ImVec2(-1, 16));
    ImGui::Spacing();

    for (int i = 0; i < (int)st.checks.size(); ++i) {
        auto& c = st.checks[i];
        ImGui::PushID(500 + i);

        if (c.passed) {
            ImGui::TextColored(accent, "[PASS]");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "[FAIL]");
        }
        ImGui::SameLine(60);
        ImGui::Text("%s", c.description);
        ImGui::SameLine(300);
        ImGui::TextDisabled("%s", c.detail);

        ImGui::PopID();
    }
    ImGui::EndChild();

    // Custom role editor
    ImGui::BeginChild("##custom_rules", ImVec2(-1, 0), true);
    ImGui::TextColored(accent, "Policy Rules (Custom Role Editor)");
    ImGui::SameLine(ImGui::GetWindowWidth() - 120);
    if (ImGui::SmallButton("Add Rule")) {
        st.show_custom_editor = true;
        memset(st.new_rule_name, 0, sizeof(st.new_rule_name));
        memset(st.new_rule_value, 0, sizeof(st.new_rule_value));
    }
    ImGui::Separator();

    if (ImGui::BeginTable("##rules", 4,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Rule", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Enforced", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableHeadersRow();

        int delete_idx = -1;
        for (int i = 0; i < (int)st.custom_rules.size(); ++i) {
            auto& r = st.custom_rules[i];
            ImGui::TableNextRow();
            ImGui::PushID(1000 + i);

            ImGui::TableSetColumnIndex(0); ImGui::Text("%s", r.name);
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##val", r.value, sizeof(r.value));
            ImGui::TableSetColumnIndex(2); ImGui::Checkbox("##enf", &r.enforced);
            ImGui::TableSetColumnIndex(3);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.1f, 0.1f, 0.8f));
            if (ImGui::SmallButton("Del")) delete_idx = i;
            ImGui::PopStyleColor();

            ImGui::PopID();
        }
        ImGui::EndTable();
        if (delete_idx >= 0) st.custom_rules.erase(st.custom_rules.begin() + delete_idx);
    }

    ImGui::Spacing();
    if (ImGui::Button("Apply All Rules", ImVec2(160, 28))) {
        st.update_checks();
    }
    ImGui::EndChild();

    // Add Rule dialog
    if (st.show_custom_editor) {
        ImGui::OpenPopup("Add Policy Rule");
        st.show_custom_editor = false;
    }
    if (ImGui::BeginPopupModal("Add Policy Rule", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("Rule Name", st.new_rule_name, 64);
        ImGui::InputText("Value", st.new_rule_value, 128);
        ImGui::Spacing();
        if (ImGui::Button("Add", ImVec2(120, 30)) && strlen(st.new_rule_name) > 0) {
            PolicyRule nr{};
            snprintf(nr.name, 64, "%s", st.new_rule_name);
            snprintf(nr.value, 128, "%s", st.new_rule_value);
            nr.enforced = true;
            st.custom_rules.push_back(nr);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 30))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

} // namespace straylight::policy
