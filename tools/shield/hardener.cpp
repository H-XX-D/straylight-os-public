// tools/shield/hardener.cpp
// Full implementation of automated security hardening.

#include "hardener.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace straylight {

Hardener::Hardener() = default;
Hardener::~Hardener() = default;

std::string Hardener::run_cmd(const std::string& cmd) {
    std::array<char, 4096> buffer{};
    std::string output;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return {};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
        output += buffer.data();
    }
    pclose(pipe);
    while (!output.empty() && output.back() == '\n') output.pop_back();
    return output;
}

bool Hardener::set_sysctl(const std::string& key, const std::string& value,
                            HardenAction& action) {
    action.before = run_cmd("sysctl -n " + key + " 2>/dev/null");
    if (action.before == value) {
        // Already set.
        action.after = value;
        action.applied = true;
        action.description += " (already set)";
        return true;
    }

    if (dry_run_) {
        action.after = value;
        action.applied = false;
        action.description += " (would change)";
        return true;
    }

    std::string result = run_cmd("sysctl -w " + key + "=" + value + " 2>&1");
    action.after = run_cmd("sysctl -n " + key + " 2>/dev/null");
    action.applied = (action.after == value);
    if (!action.applied) {
        action.error = "sysctl write failed: " + result;
    }

    // Also persist to /etc/sysctl.d/99-straylight-shield.conf.
    if (action.applied) {
        std::string conf_path = "/etc/sysctl.d/99-straylight-shield.conf";
        std::ofstream conf(conf_path, std::ios::app);
        if (conf.is_open()) {
            conf << key << " = " << value << "\n";
        }
    }

    return action.applied;
}

bool Hardener::set_ssh_config(const std::string& key, const std::string& value,
                                HardenAction& action) {
    std::string config_path = "/etc/ssh/sshd_config";
    std::string backup_path = config_path + ".shield-backup";

    // Read current config.
    std::ifstream in(config_path);
    if (!in.is_open()) {
        action.error = "cannot read " + config_path;
        return false;
    }
    std::vector<std::string> lines;
    std::string line;
    bool found = false;
    while (std::getline(in, line)) {
        // Check if this line sets the key.
        std::string trimmed = line;
        while (!trimmed.empty() && trimmed[0] == ' ') trimmed.erase(0, 1);
        if (trimmed.substr(0, key.size()) == key) {
            auto space = trimmed.find(' ', key.size());
            if (space != std::string::npos || trimmed.size() == key.size()) {
                action.before = trimmed;
                if (!dry_run_) {
                    line = key + " " + value;
                }
                found = true;
            }
        }
        lines.push_back(line);
    }
    in.close();

    if (!found) {
        action.before = "(not set)";
        if (!dry_run_) {
            lines.push_back(key + " " + value);
        }
    }

    action.after = key + " " + value;

    if (dry_run_) {
        action.applied = false;
        action.description += " (would change)";
        return true;
    }

    // Backup original.
    std::error_code ec;
    if (!fs::exists(backup_path, ec)) {
        fs::copy_file(config_path, backup_path, ec);
    }

    // Write new config.
    std::ofstream out(config_path);
    if (!out.is_open()) {
        action.error = "cannot write " + config_path;
        return false;
    }
    for (const auto& l : lines) {
        out << l << "\n";
    }
    out.close();

    action.applied = true;
    return true;
}

// ---------------------------------------------------------------------------
// Sysctl hardening
// ---------------------------------------------------------------------------

std::vector<HardenAction> Hardener::harden_sysctl(Level level) {
    std::vector<HardenAction> actions;

    struct SysctlRule {
        std::string key;
        std::string value;
        std::string desc;
        Level min_level;
    };

    std::vector<SysctlRule> rules = {
        // Basic.
        {"kernel.randomize_va_space", "2", "Enable full ASLR", Level::Basic},
        {"fs.suid_dumpable", "0", "Disable SUID core dumps", Level::Basic},
        {"net.ipv4.ip_forward", "0", "Disable IP forwarding", Level::Basic},

        // Moderate.
        {"kernel.dmesg_restrict", "1", "Restrict dmesg to root", Level::Moderate},
        {"kernel.kptr_restrict", "2", "Hide kernel pointers", Level::Moderate},
        {"kernel.sysrq", "0", "Disable magic SysRq key", Level::Moderate},
        {"net.ipv4.conf.all.accept_redirects", "0", "Disable ICMP redirects", Level::Moderate},
        {"net.ipv4.conf.all.send_redirects", "0", "Disable send redirects", Level::Moderate},
        {"net.ipv4.conf.all.accept_source_route", "0", "Disable source routing", Level::Moderate},
        {"net.ipv4.tcp_syncookies", "1", "Enable SYN cookies", Level::Moderate},

        // Strict.
        {"net.ipv4.conf.all.log_martians", "1", "Log martian packets", Level::Strict},
        {"net.ipv4.conf.default.accept_redirects", "0", "Disable default ICMP redirects", Level::Strict},
        {"net.ipv6.conf.all.accept_redirects", "0", "Disable IPv6 ICMP redirects", Level::Strict},
        {"net.ipv4.icmp_echo_ignore_broadcasts", "1", "Ignore broadcast pings", Level::Strict},
        {"kernel.unprivileged_bpf_disabled", "1", "Disable unprivileged BPF", Level::Strict},
        {"kernel.perf_event_paranoid", "3", "Restrict perf events", Level::Strict},
    };

    for (const auto& rule : rules) {
        if (static_cast<int>(level) < static_cast<int>(rule.min_level)) continue;

        HardenAction action;
        action.category = "sysctl";
        action.description = rule.desc + " (" + rule.key + "=" + rule.value + ")";
        set_sysctl(rule.key, rule.value, action);
        actions.push_back(std::move(action));
    }

    return actions;
}

// ---------------------------------------------------------------------------
// Service hardening
// ---------------------------------------------------------------------------

std::vector<HardenAction> Hardener::harden_services(Level level) {
    std::vector<HardenAction> actions;

    std::vector<std::pair<std::string, Level>> to_disable = {
        {"telnet.socket", Level::Basic},
        {"rsh.socket", Level::Basic},
        {"rlogin.socket", Level::Basic},
        {"rexec.socket", Level::Basic},
        {"vsftpd", Level::Moderate},
        {"xinetd", Level::Moderate},
        {"avahi-daemon", Level::Strict},
    };

    for (const auto& [svc, min_level] : to_disable) {
        if (static_cast<int>(level) < static_cast<int>(min_level)) continue;

        HardenAction action;
        action.category = "services";
        action.description = "Disable " + svc;

        std::string status = run_cmd("systemctl is-active " + svc + " 2>/dev/null");
        action.before = status;

        if (status != "active") {
            action.after = "already inactive";
            action.applied = true;
            action.description += " (already inactive)";
        } else if (dry_run_) {
            action.after = "would disable";
            action.applied = false;
        } else {
            run_cmd("systemctl disable --now " + svc + " 2>/dev/null");
            action.after = run_cmd("systemctl is-active " + svc + " 2>/dev/null");
            action.applied = (action.after != "active");
            if (!action.applied) {
                action.error = "failed to disable " + svc;
            }
        }

        actions.push_back(std::move(action));
    }

    return actions;
}

// ---------------------------------------------------------------------------
// Filesystem hardening
// ---------------------------------------------------------------------------

std::vector<HardenAction> Hardener::harden_filesystem(Level level) {
    std::vector<HardenAction> actions;

    // Fix /tmp permissions.
    {
        HardenAction action;
        action.category = "filesystem";
        action.description = "Set /tmp permissions to 1777";
        action.before = run_cmd("stat -c '%a' /tmp 2>/dev/null");

        if (action.before == "1777") {
            action.after = "1777";
            action.applied = true;
            action.description += " (already correct)";
        } else if (dry_run_) {
            action.after = "1777";
            action.applied = false;
        } else {
            run_cmd("chmod 1777 /tmp 2>/dev/null");
            action.after = run_cmd("stat -c '%a' /tmp 2>/dev/null");
            action.applied = (action.after == "1777");
        }
        actions.push_back(std::move(action));
    }

    // Strict: remove world-writable from /etc files.
    if (level >= Level::Moderate) {
        HardenAction action;
        action.category = "filesystem";
        action.description = "Remove world-writable from /etc";
        std::string ww = run_cmd(
            "find /etc -xdev -type f -perm -0002 2>/dev/null | wc -l");
        action.before = ww + " world-writable files in /etc";
        if (dry_run_) {
            action.applied = false;
        } else {
            run_cmd("find /etc -xdev -type f -perm -0002 -exec chmod o-w {} \\; 2>/dev/null");
            std::string after = run_cmd(
                "find /etc -xdev -type f -perm -0002 2>/dev/null | wc -l");
            action.after = after + " world-writable files remain";
            action.applied = true;
        }
        actions.push_back(std::move(action));
    }

    return actions;
}

// ---------------------------------------------------------------------------
// Firewall hardening
// ---------------------------------------------------------------------------

std::vector<HardenAction> Hardener::harden_firewall(Level level) {
    std::vector<HardenAction> actions;
    (void)level;

    HardenAction action;
    action.category = "firewall";
    action.description = "Enable UFW with default deny incoming";

    std::string status = run_cmd("ufw status 2>/dev/null");
    action.before = status.empty() ? "ufw not installed" : status;

    if (status.find("Status: active") != std::string::npos) {
        action.after = "already active";
        action.applied = true;
        action.description += " (already active)";
    } else if (dry_run_) {
        action.after = "would enable ufw";
        action.applied = false;
    } else {
        // Check if ufw is available.
        std::string which = run_cmd("which ufw 2>/dev/null");
        if (which.empty()) {
            action.error = "ufw not installed";
            action.applied = false;
        } else {
            run_cmd("ufw default deny incoming 2>/dev/null");
            run_cmd("ufw default allow outgoing 2>/dev/null");
            // Allow SSH so we don't lock ourselves out.
            run_cmd("ufw allow ssh 2>/dev/null");
            run_cmd("ufw --force enable 2>/dev/null");
            action.after = run_cmd("ufw status 2>/dev/null");
            action.applied = (action.after.find("active") != std::string::npos);
        }
    }

    actions.push_back(std::move(action));
    return actions;
}

// ---------------------------------------------------------------------------
// SSH hardening
// ---------------------------------------------------------------------------

std::vector<HardenAction> Hardener::harden_ssh(Level level) {
    std::vector<HardenAction> actions;

    struct SSHRule {
        std::string key;
        std::string value;
        std::string desc;
        Level min_level;
    };

    std::vector<SSHRule> rules = {
        {"PermitRootLogin", "no", "Disable SSH root login", Level::Basic},
        {"MaxAuthTries", "3", "Limit SSH auth attempts", Level::Basic},
        {"PasswordAuthentication", "no", "Disable password auth (key-only)", Level::Moderate},
        {"PermitEmptyPasswords", "no", "Disallow empty passwords", Level::Basic},
        {"X11Forwarding", "no", "Disable X11 forwarding", Level::Moderate},
        {"ClientAliveInterval", "300", "Set SSH keep-alive interval", Level::Moderate},
        {"ClientAliveCountMax", "2", "Limit SSH keep-alive count", Level::Moderate},
        {"AllowAgentForwarding", "no", "Disable agent forwarding", Level::Strict},
        {"AllowTcpForwarding", "no", "Disable TCP forwarding", Level::Strict},
    };

    for (const auto& rule : rules) {
        if (static_cast<int>(level) < static_cast<int>(rule.min_level)) continue;

        HardenAction action;
        action.category = "ssh";
        action.description = rule.desc + " (" + rule.key + " " + rule.value + ")";
        set_ssh_config(rule.key, rule.value, action);
        actions.push_back(std::move(action));
    }

    // Restart sshd if any changes were applied.
    if (!dry_run_) {
        bool any_applied = false;
        for (const auto& a : actions) {
            if (a.applied && a.before != a.after) {
                any_applied = true;
                break;
            }
        }
        if (any_applied) {
            HardenAction restart;
            restart.category = "ssh";
            restart.description = "Restart sshd to apply changes";
            restart.before = "running";
            run_cmd("systemctl restart sshd 2>/dev/null || systemctl restart ssh 2>/dev/null");
            restart.after = run_cmd("systemctl is-active sshd 2>/dev/null || "
                                     "systemctl is-active ssh 2>/dev/null");
            restart.applied = true;
            actions.push_back(std::move(restart));
        }
    }

    return actions;
}

// ---------------------------------------------------------------------------
// Audit daemon hardening
// ---------------------------------------------------------------------------

std::vector<HardenAction> Hardener::harden_audit(Level level) {
    std::vector<HardenAction> actions;
    if (level < Level::Moderate) return actions;

    HardenAction action;
    action.category = "audit";
    action.description = "Enable auditd (Linux audit daemon)";

    std::string status = run_cmd("systemctl is-active auditd 2>/dev/null");
    action.before = status;

    if (status == "active") {
        action.after = "already active";
        action.applied = true;
        action.description += " (already active)";
    } else if (dry_run_) {
        action.after = "would enable";
        action.applied = false;
    } else {
        run_cmd("systemctl enable --now auditd 2>/dev/null");
        action.after = run_cmd("systemctl is-active auditd 2>/dev/null");
        action.applied = (action.after == "active");
        if (!action.applied) {
            action.error = "auditd may not be installed";
        }
    }

    actions.push_back(std::move(action));
    return actions;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

Result<HardenReport, std::string> Hardener::harden(Level level) {
    dry_run_ = false;
    HardenReport report;

    switch (level) {
        case Level::Basic:    report.level = "basic"; break;
        case Level::Moderate: report.level = "moderate"; break;
        case Level::Strict:   report.level = "strict"; break;
    }

    auto add = [&](std::vector<HardenAction>&& acts) {
        for (auto& a : acts) {
            if (a.applied) report.applied++;
            else if (!a.error.empty()) report.failed++;
            else report.skipped++;
            report.actions.push_back(std::move(a));
        }
    };

    add(harden_sysctl(level));
    add(harden_services(level));
    add(harden_filesystem(level));
    add(harden_firewall(level));
    add(harden_ssh(level));
    add(harden_audit(level));

    return Result<HardenReport, std::string>::ok(std::move(report));
}

Result<HardenReport, std::string> Hardener::preview(Level level) {
    dry_run_ = true;
    auto result = harden(level);
    dry_run_ = false;
    return result;
}

std::string Hardener::format_report(const HardenReport& report) {
    std::ostringstream out;

    out << "\n";
    out << "  StrayLight Shield - Hardening Report\n";
    out << "  " << std::string(40, '=') << "\n\n";
    out << "  Level: " << report.level << "\n";
    out << "  Applied: " << report.applied
        << "  Skipped: " << report.skipped
        << "  Failed: " << report.failed << "\n\n";

    std::string current_cat;
    for (const auto& a : report.actions) {
        if (a.category != current_cat) {
            current_cat = a.category;
            out << "  --- " << current_cat << " ---\n";
        }

        std::string status;
        if (a.applied) status = "[OK]  ";
        else if (!a.error.empty()) status = "[FAIL]";
        else status = "[SKIP]";

        out << "  " << status << " " << a.description << "\n";
        if (!a.before.empty() || !a.after.empty()) {
            out << "         before: " << a.before << "\n";
            out << "         after:  " << a.after << "\n";
        }
        if (!a.error.empty()) {
            out << "         error:  " << a.error << "\n";
        }
        out << "\n";
    }

    return out.str();
}

Result<Hardener::Level, std::string>
Hardener::parse_level(const std::string& s) {
    if (s == "basic") return Result<Level, std::string>::ok(Level::Basic);
    if (s == "moderate") return Result<Level, std::string>::ok(Level::Moderate);
    if (s == "strict") return Result<Level, std::string>::ok(Level::Strict);
    return Result<Level, std::string>::error(
        "invalid level: " + s + " (valid: basic, moderate, strict)");
}

} // namespace straylight
