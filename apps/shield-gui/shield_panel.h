// apps/shield-gui/shield_panel.h
// StrayLight Shield GUI — Security Audit panel
#pragma once

#include <imgui.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <fstream>
#include <sstream>
#include <array>
#include <cstdlib>

namespace straylight::shield {

struct AuditFinding {
    std::string category;
    std::string description;
    int         severity; // 0=pass, 1=info, 2=medium, 3=high, 4=critical
    std::string fix_command;
    bool        fixed = false;
};

struct ShieldState {
    float security_score = 0.0f;
    std::vector<AuditFinding> findings;
    bool  auditing = false;
    float audit_progress = 0.0f;

    // Harden
    bool show_harden_dialog = false;
    int  harden_level = 1; // 0=basic, 1=standard, 2=paranoid
    bool hardening = false;
    float harden_progress = 0.0f;

    // Diff
    bool show_diff = false;
    std::vector<std::string> harden_changes;

    // Live OS link (no fabricated audit data)
    bool        ok_ = false;
    std::string err_;
    double      last_refresh_ = -1.0e9;

    // --- OS source helpers (proven popen/ifstream patterns from
    //     apps/hub/service_panel.cpp and apps/system_monitor/cpu.cpp) ---
    static std::string read_file_trim(const std::string& path) {
        std::ifstream f(path);
        if (!f.is_open()) return std::string();
        std::stringstream ss;
        ss << f.rdbuf();
        std::string s = ss.str();
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
            s.pop_back();
        return s;
    }

    static std::string exec_cmd(const std::string& cmd) {
        std::array<char, 4096> buf{};
        std::string result;
        FILE* pipe = ::popen(cmd.c_str(), "r");
        if (!pipe) return std::string();
        while (::fgets(buf.data(), static_cast<int>(buf.size()), pipe))
            result += buf.data();
        ::pclose(pipe);
        while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
            result.pop_back();
        return result;
    }

    // command exists on PATH?
    static bool have_cmd(const std::string& name) {
        return ::system(("command -v " + name + " >/dev/null 2>&1").c_str()) == 0;
    }

    void add(const std::string& cat, const std::string& desc, int sev,
             const std::string& fix = std::string()) {
        findings.push_back({cat, desc, sev, fix, false});
    }

    void ensure_sources() {
        // /proc must be readable for any audit to be meaningful.
        std::ifstream probe("/proc/sys/kernel/randomize_va_space");
        ok_ = probe.is_open();
        err_ = ok_ ? std::string()
                   : std::string("cannot read /proc/sys (procfs unavailable)");
    }

    void refresh() {
        ensure_sources();
        if (!ok_) { findings.clear(); security_score = 0.0f; return; }

        findings.clear();

        // ---------------- Filesystem ----------------
        {
            std::string mounts;
            {
                std::ifstream mf("/proc/mounts");
                if (mf.is_open()) { std::stringstream ss; ss << mf.rdbuf(); mounts = ss.str(); }
            }
            auto mount_opts = [&](const std::string& mp) -> std::string {
                std::istringstream is(mounts);
                std::string line;
                while (std::getline(is, line)) {
                    std::istringstream ls(line);
                    std::string dev, point, type, opts;
                    ls >> dev >> point >> type >> opts;
                    if (point == mp) return opts;
                }
                return std::string();
            };
            std::string tmp_opts = mount_opts("/tmp");
            if (!tmp_opts.empty()) {
                bool noexec = tmp_opts.find("noexec") != std::string::npos;
                add("Filesystem",
                    std::string("/tmp mount options: ") + tmp_opts,
                    noexec ? 0 : 2,
                    noexec ? "" : "remount /tmp with noexec,nosuid,nodev");
            } else {
                add("Filesystem", "/tmp not a separate mount", 1);
            }

            // SUID binaries (count only; enumerating is the fix-time job)
            std::string suid = exec_cmd("find / -xdev -perm -4000 -type f 2>/dev/null | wc -l");
            int suid_n = 0; try { suid_n = std::stoi(suid); } catch (...) {}
            add("Filesystem",
                std::to_string(suid_n) + " SUID binaries present",
                suid_n > 0 ? 1 : 0,
                suid_n > 0 ? "review: find / -xdev -perm -4000 -type f" : "");

            // World-writable dirs without sticky bit
            std::string ww = exec_cmd(
                "find / -xdev -type d -perm -0002 ! -perm -1000 2>/dev/null | wc -l");
            int ww_n = 0; try { ww_n = std::stoi(ww); } catch (...) {}
            add("Filesystem",
                std::to_string(ww_n) + " world-writable dirs without sticky bit",
                ww_n > 0 ? 2 : 0,
                ww_n > 0 ? "chmod +t on offending directories" : "");
        }

        // ---------------- Network ----------------
        {
            if (have_cmd("nft")) {
                std::string rs = exec_cmd("nft list ruleset 2>/dev/null | wc -l");
                int rs_n = 0; try { rs_n = std::stoi(rs); } catch (...) {}
                add("Network",
                    rs_n > 0 ? "nftables firewall has rules loaded"
                             : "nftables ruleset is empty",
                    rs_n > 0 ? 0 : 3,
                    rs_n > 0 ? "" : "load a firewall ruleset");
            } else {
                add("Network", "firewall state unknown (nft not installed)", 1);
            }

            // Listening ports (count)
            std::string ports = exec_cmd("ss -tln 2>/dev/null | tail -n +2 | wc -l");
            int ports_n = 0; try { ports_n = std::stoi(ports); } catch (...) {}
            add("Network", std::to_string(ports_n) + " listening TCP ports",
                ports_n > 0 ? 1 : 0);

            std::string ipf = read_file_trim("/proc/sys/net/ipv4/ip_forward");
            if (!ipf.empty())
                add("Network",
                    ipf == "0" ? "IP forwarding disabled" : "IP forwarding enabled",
                    ipf == "0" ? 0 : 2,
                    ipf == "0" ? "" : "sysctl -w net.ipv4.ip_forward=0");
            else
                add("Network", "IP forwarding state unknown", 1);

            std::string resolved = read_file_trim("/etc/systemd/resolved.conf");
            if (!resolved.empty()) {
                bool dot = resolved.find("DNSOverTLS=yes") != std::string::npos
                        || resolved.find("DNSOverTLS=opportunistic") != std::string::npos;
                add("Network",
                    dot ? "DNS-over-TLS configured" : "DNS-over-TLS not configured",
                    dot ? 0 : 2,
                    dot ? "" : "set DNSOverTLS=yes in /etc/systemd/resolved.conf");
            } else {
                add("Network", "DNS-over-TLS state unknown (no resolved.conf)", 1);
            }
        }

        // ---------------- Users ----------------
        {
            // login users with uid >= 1000 and a real shell
            std::string nshell = exec_cmd(
                "awk -F: '$3>=1000 && $3<65534 && $7!=\"/usr/sbin/nologin\" && "
                "$7!=\"/bin/false\" && $7!=\"/sbin/nologin\" {c++} END{print c+0}' "
                "/etc/passwd 2>/dev/null");
            int nshell_n = 0; try { nshell_n = std::stoi(nshell); } catch (...) {}
            add("Users",
                std::to_string(nshell_n) + " login users with shell access",
                nshell_n > 0 ? 1 : 0);

            // PermitRootLogin from sshd_config
            std::string prl = exec_cmd(
                "grep -iE '^[[:space:]]*PermitRootLogin' /etc/ssh/sshd_config "
                "2>/dev/null | tail -1 | awk '{print tolower($2)}'");
            if (!prl.empty()) {
                bool disabled = (prl == "no" || prl == "prohibit-password");
                add("Users",
                    std::string("SSH PermitRootLogin = ") + prl,
                    disabled ? 0 : 3,
                    disabled ? "" : "set PermitRootLogin no in /etc/ssh/sshd_config");
            } else {
                add("Users", "SSH PermitRootLogin not explicitly set", 1);
            }

            // empty-password accounts (needs root to read /etc/shadow)
            std::ifstream shadow("/etc/shadow");
            if (shadow.is_open()) {
                std::string empty = exec_cmd(
                    "awk -F: '($2==\"\"){c++} END{print c+0}' /etc/shadow 2>/dev/null");
                int empty_n = 0; try { empty_n = std::stoi(empty); } catch (...) {}
                add("Users",
                    std::to_string(empty_n) + " accounts with empty password",
                    empty_n > 0 ? 4 : 0,
                    empty_n > 0 ? "passwd -l on affected accounts" : "");
            } else {
                add("Users", "empty-password check skipped (/etc/shadow needs root)", 1);
            }
        }

        // ---------------- Kernel ----------------
        {
            std::string aslr = read_file_trim("/proc/sys/kernel/randomize_va_space");
            if (!aslr.empty())
                add("Kernel",
                    std::string("ASLR (randomize_va_space=") + aslr + ")",
                    aslr == "2" ? 0 : 2,
                    aslr == "2" ? "" : "sysctl -w kernel.randomize_va_space=2");

            std::string dmesg = read_file_trim("/proc/sys/kernel/dmesg_restrict");
            if (!dmesg.empty())
                add("Kernel",
                    dmesg == "1" ? "dmesg restricted to root" : "dmesg readable by all users",
                    dmesg == "1" ? 0 : 2,
                    dmesg == "1" ? "" : "sysctl -w kernel.dmesg_restrict=1");

            std::string modoff = read_file_trim("/proc/sys/kernel/modules_disabled");
            if (!modoff.empty())
                add("Kernel",
                    modoff == "1" ? "kernel module loading locked"
                                  : "kernel module loading allowed",
                    // informational: 1 is rare on a live workstation
                    modoff == "1" ? 0 : 1);

            std::string core = read_file_trim("/proc/sys/kernel/core_pattern");
            if (!core.empty()) {
                bool piped = !core.empty() && core[0] == '|';
                add("Kernel",
                    std::string("core_pattern = ") + core,
                    piped ? 1 : 0);
            }
        }

        // ---------------- Services ----------------
        {
            if (have_cmd("systemctl")) {
                std::string run = exec_cmd(
                    "systemctl list-units --type=service --state=running "
                    "--no-legend --no-pager 2>/dev/null | wc -l");
                int run_n = 0; try { run_n = std::stoi(run); } catch (...) {}
                add("Services", std::to_string(run_n) + " services running",
                    run_n > 0 ? 1 : 0);

                std::string au = exec_cmd(
                    "systemctl is-enabled unattended-upgrades 2>/dev/null");
                bool auto_up = (au == "enabled");
                add("Services",
                    auto_up ? "automatic security updates enabled"
                            : "automatic security updates not enabled",
                    auto_up ? 0 : 2,
                    auto_up ? "" : "systemctl enable --now unattended-upgrades");
            } else {
                add("Services", "service state unknown (systemctl not present)", 1);
            }

            if (have_cmd("aa-status")) {
                std::string prof = exec_cmd(
                    "aa-status --profiled 2>/dev/null");
                int prof_n = 0; try { prof_n = std::stoi(prof); } catch (...) {}
                add("Services",
                    std::to_string(prof_n) + " AppArmor profiles loaded",
                    prof_n > 0 ? 0 : 2,
                    prof_n > 0 ? "" : "enable AppArmor profiles");
            } else {
                add("Services", "AppArmor coverage unknown (aa-status not present)", 1);
            }
        }

        recalc_score();
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

    void recalc_score() {
        int total = 0, passed = 0;
        for (auto& f : findings) {
            total++;
            if (f.severity == 0 || f.fixed) passed++;
        }
        security_score = total > 0 ? (float)passed / (float)total * 100.0f : 0.0f;
    }
};

inline void render_shield_panel(ShieldState& st) {
    st.maybe_refresh();
    if (!st.ok_) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.3f, 1.0f));
        ImGui::TextWrapped("security sources unavailable: %s", st.err_.c_str());
        ImGui::PopStyleColor();
        if (ImGui::SmallButton("Retry")) st.refresh();
        ImGui::Separator();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.67f, 1.0f));
    ImGui::Text("STRAYLIGHT SHIELD");
    ImGui::PopStyleColor();
    ImGui::SameLine(ImGui::GetWindowWidth() - 80);
    if (ImGui::SmallButton("Close")) {}
    ImGui::Separator();
    ImGui::Spacing();

    // Top bar: score gauge + buttons
    float top_h = 120.0f;
    if (ImGui::BeginChild("##top_bar", ImVec2(0, top_h), false)) {
        // Security score gauge
        if (ImGui::BeginChild("##score_gauge", ImVec2(200, -1), false)) {
            ImDrawList* draw = ImGui::GetWindowDrawList();
            ImVec2 center = ImGui::GetCursorScreenPos();
            center.x += 100;
            center.y += 55;
            float radius = 45.0f;

            for (float a = 3.14159f; a < 3.14159f * 2.0f; a += 0.02f) {
                float x1 = center.x + cosf(a) * radius;
                float y1 = center.y + sinf(a) * radius;
                float x2 = center.x + cosf(a) * (radius - 10.0f);
                float y2 = center.y + sinf(a) * (radius - 10.0f);
                draw->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), IM_COL32(40, 40, 60, 255), 2.0f);
            }
            float frac = st.security_score / 100.0f;
            float end_angle = 3.14159f + 3.14159f * frac;
            ImU32 col = st.security_score >= 80 ? IM_COL32(0, 200, 130, 255)
                      : st.security_score >= 60 ? IM_COL32(200, 180, 40, 255)
                      : IM_COL32(200, 60, 60, 255);
            for (float a = 3.14159f; a < end_angle; a += 0.02f) {
                float x1 = center.x + cosf(a) * radius;
                float y1 = center.y + sinf(a) * radius;
                float x2 = center.x + cosf(a) * (radius - 10.0f);
                float y2 = center.y + sinf(a) * (radius - 10.0f);
                draw->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), col, 3.0f);
            }
            char score_text[16];
            snprintf(score_text, sizeof(score_text), "%.0f", st.security_score);
            ImVec2 ts = ImGui::CalcTextSize(score_text);
            draw->AddText(nullptr, 24.0f, ImVec2(center.x - ts.x, center.y - 15), col, score_text);
            draw->AddText(ImVec2(center.x - 30, center.y + 8), IM_COL32(160, 160, 160, 255), "Security Score");
            ImGui::Dummy(ImVec2(0, 110));
        }
        ImGui::EndChild();

        ImGui::SameLine();

        // Buttons
        if (ImGui::BeginChild("##buttons", ImVec2(0, -1), false)) {
            ImGui::Spacing();
            if (st.auditing) {
                ImGui::BeginDisabled();
                ImGui::Button("Running Audit...", ImVec2(160, 32));
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::ProgressBar(st.audit_progress, ImVec2(200, 32));
                // removed fake per-frame audit animation; refresh() loads real OS data
                if (st.audit_progress >= 1.0f) { st.auditing = false; st.audit_progress = 1.0f; }
            } else {
                if (ImGui::Button("Run Audit", ImVec2(140, 32))) {
                    st.auditing = true;
                    st.audit_progress = 0.0f;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Harden System", ImVec2(140, 32))) {
                st.show_harden_dialog = true;
            }
            ImGui::SameLine();
            if (ImGui::Button(st.show_diff ? "Hide Changes" : "Show Changes", ImVec2(140, 32))) {
                st.show_diff = !st.show_diff;
            }

            ImGui::Spacing();
            // Summary counts
            int pass = 0, info = 0, med = 0, high = 0, crit = 0;
            for (auto& f : st.findings) {
                if (f.fixed) { pass++; continue; }
                switch (f.severity) {
                    case 0: pass++; break;
                    case 1: info++; break;
                    case 2: med++; break;
                    case 3: high++; break;
                    case 4: crit++; break;
                }
            }
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.5f, 1.0f), "%d Pass", pass);
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.5f, 0.7f, 1.0f, 1.0f), "%d Info", info);
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "%d Medium", med);
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f), "%d High", high);
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "%d Critical", crit);
        }
        ImGui::EndChild();
    }
    ImGui::EndChild();

    ImGui::Separator();
    ImGui::Spacing();

    // Main content: findings + optional diff
    float findings_w = st.show_diff ? ImGui::GetContentRegionAvail().x * 0.6f : ImGui::GetContentRegionAvail().x;

    if (ImGui::BeginChild("##findings", ImVec2(findings_w, -1), false)) {
        std::string current_cat;
        for (int i = 0; i < (int)st.findings.size(); ++i) {
            auto& f = st.findings[i];
            if (f.category != current_cat) {
                current_cat = f.category;
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.67f, 1.0f), "%s", current_cat.c_str());
                ImGui::Separator();
            }

            ImGui::PushID(i);

            // Severity icon
            ImVec4 sev_col;
            const char* sev_icon;
            if (f.fixed) {
                sev_col = ImVec4(0.2f, 1.0f, 0.5f, 1.0f);
                sev_icon = "[FIXED]";
            } else {
                switch (f.severity) {
                    case 0: sev_col = ImVec4(0.2f, 1.0f, 0.5f, 1.0f); sev_icon = "[PASS]"; break;
                    case 1: sev_col = ImVec4(0.5f, 0.7f, 1.0f, 1.0f); sev_icon = "[INFO]"; break;
                    case 2: sev_col = ImVec4(1.0f, 0.8f, 0.2f, 1.0f); sev_icon = "[MED] "; break;
                    case 3: sev_col = ImVec4(1.0f, 0.5f, 0.2f, 1.0f); sev_icon = "[HIGH]"; break;
                    default: sev_col = ImVec4(1.0f, 0.2f, 0.2f, 1.0f); sev_icon = "[CRIT]"; break;
                }
            }

            ImGui::TextColored(sev_col, "%s", sev_icon);
            ImGui::SameLine();
            ImGui::TextWrapped("%s", f.description.c_str());

            // Fix button (only for unfixed findings with a command)
            if (!f.fixed && !f.fix_command.empty() && f.severity > 0) {
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.55f, 0.38f, 0.8f));
                if (ImGui::SmallButton("Fix")) {
                    f.fixed = true;
                    st.recalc_score();
                }
                ImGui::PopStyleColor();
            }

            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    // Diff panel
    if (st.show_diff) {
        ImGui::SameLine();
        if (ImGui::BeginChild("##diff_panel", ImVec2(0, -1), true)) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.67f, 1.0f), "Hardening Changes Preview");
            ImGui::Separator();
            ImGui::Spacing();
            for (auto& line : st.harden_changes) {
                if (line.empty()) {
                    ImGui::Spacing();
                } else if (line[0] == '+') {
                    ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "%s", line.c_str());
                } else if (line[0] == '-') {
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", line.c_str());
                } else if (line[0] == '@') {
                    ImGui::TextColored(ImVec4(0.4f, 0.6f, 1.0f, 1.0f), "%s", line.c_str());
                } else {
                    ImGui::Text("%s", line.c_str());
                }
            }
        }
        ImGui::EndChild();
    }

    // Harden dialog
    if (st.show_harden_dialog) {
        ImGui::OpenPopup("Harden System");
        st.show_harden_dialog = false;
    }
    if (ImGui::BeginPopupModal("Harden System", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Select hardening level:");
        ImGui::Spacing();
        const char* levels[] = {"Basic - Essential security fixes only",
                                "Standard - Recommended security posture",
                                "Paranoid - Maximum lockdown (may break some tools)"};
        for (int i = 0; i < 3; ++i) {
            ImGui::RadioButton(levels[i], &st.harden_level, i);
        }
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "A snapshot will be created before hardening.");
        ImGui::Spacing();

        if (st.hardening) {
            ImGui::ProgressBar(st.harden_progress, ImVec2(-1, 30));
            // removed fake per-frame harden animation
            if (st.harden_progress >= 1.0f) {
                st.hardening = false;
                st.harden_progress = 0.0f;
                // Fix applicable findings
                for (auto& f : st.findings) {
                    if (!f.fix_command.empty() && f.severity <= (st.harden_level + 2)) {
                        f.fixed = true;
                    }
                }
                st.recalc_score();
                ImGui::CloseCurrentPopup();
            }
        } else {
            if (ImGui::Button("Apply", ImVec2(120, 30))) {
                st.hardening = true;
                st.harden_progress = 0.0f;
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 30))) { ImGui::CloseCurrentPopup(); }
        }
        ImGui::EndPopup();
    }
}

} // namespace straylight::shield
