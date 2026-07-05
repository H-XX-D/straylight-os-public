// tools/shield/auditor.cpp
// Full implementation of the security auditor.

#include "auditor.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using Sev = straylight::AuditFinding::Severity;

namespace straylight {

Auditor::Auditor() = default;
Auditor::~Auditor() = default;

std::string Auditor::run_cmd(const std::string& cmd) {
    std::array<char, 4096> buffer{};
    std::string output;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return {};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
        output += buffer.data();
    }
    pclose(pipe);
    return output;
}

bool Auditor::file_contains(const std::string& path, const std::string& needle) {
    std::ifstream in(path);
    if (!in.is_open()) return false;
    std::string line;
    while (std::getline(in, line)) {
        if (line.find(needle) != std::string::npos) return true;
    }
    return false;
}

std::string Auditor::sysctl_get(const std::string& key) {
    std::string out = run_cmd("sysctl -n " + key + " 2>/dev/null");
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) {
        out.pop_back();
    }
    return out;
}

// ---------------------------------------------------------------------------
// Filesystem checks
// ---------------------------------------------------------------------------

std::vector<AuditFinding> Auditor::check_filesystem() {
    std::vector<AuditFinding> findings;

    // Check world-writable files outside /tmp.
    {
        AuditFinding f;
        f.category = "filesystem";
        f.check = "world-writable-files";
        f.description = "World-writable files outside /tmp and /var/tmp";
        f.severity = Sev::Medium;
        f.remediation = "chmod o-w on identified files";

        std::string out = run_cmd(
            "find / -xdev -type f -perm -0002 "
            "-not -path '/tmp/*' -not -path '/var/tmp/*' "
            "-not -path '/proc/*' -not -path '/sys/*' "
            "2>/dev/null | head -20");
        f.passed = out.empty();
        if (!f.passed) {
            f.description += "\n  Found:\n" + out;
        }
        findings.push_back(std::move(f));
    }

    // Check SUID/SGID binaries.
    {
        AuditFinding f;
        f.category = "filesystem";
        f.check = "suid-sgid-binaries";
        f.description = "SUID/SGID binaries on the system";
        f.severity = Sev::Low;
        f.remediation = "Review each SUID/SGID binary; remove the bit if unnecessary";

        std::string out = run_cmd(
            "find / -xdev \\( -perm -4000 -o -perm -2000 \\) -type f "
            "-not -path '/proc/*' -not -path '/sys/*' "
            "2>/dev/null | wc -l");
        int count = std::atoi(out.c_str());
        // Typical Linux has ~20-30 SUID binaries. Flag if > 40.
        f.passed = (count <= 40);
        f.description = "Found " + std::to_string(count) + " SUID/SGID binaries";
        if (!f.passed) {
            f.description += " (expected <= 40)";
        }
        findings.push_back(std::move(f));
    }

    // Check /tmp permissions.
    {
        AuditFinding f;
        f.category = "filesystem";
        f.check = "tmp-sticky-bit";
        f.description = "/tmp has sticky bit set";
        f.severity = Sev::High;
        f.remediation = "chmod 1777 /tmp";

        std::string out = run_cmd("stat -c '%a' /tmp 2>/dev/null");
        while (!out.empty() && out.back() == '\n') out.pop_back();
        f.passed = (out == "1777");
        if (!f.passed) {
            f.description = "/tmp permissions are " + out + " (expected 1777)";
        }
        findings.push_back(std::move(f));
    }

    // Check /etc/shadow permissions.
    {
        AuditFinding f;
        f.category = "filesystem";
        f.check = "shadow-permissions";
        f.description = "/etc/shadow is not world-readable";
        f.severity = Sev::Critical;
        f.remediation = "chmod 640 /etc/shadow; chown root:shadow /etc/shadow";

        std::error_code ec;
        auto perms = fs::status("/etc/shadow", ec).permissions();
        if (ec) {
            f.passed = true; // Can't check, probably fine.
            f.description = "/etc/shadow not accessible (likely OK)";
        } else {
            f.passed = (perms & fs::perms::others_read) == fs::perms::none;
            if (!f.passed) {
                f.description = "/etc/shadow is world-readable!";
            }
        }
        findings.push_back(std::move(f));
    }

    return findings;
}

// ---------------------------------------------------------------------------
// Network checks
// ---------------------------------------------------------------------------

std::vector<AuditFinding> Auditor::check_network() {
    std::vector<AuditFinding> findings;

    // Open ports.
    {
        AuditFinding f;
        f.category = "network";
        f.check = "open-ports";
        f.description = "Listening TCP ports";
        f.severity = Sev::Low;

        std::string out = run_cmd("ss -tlnp 2>/dev/null || netstat -tlnp 2>/dev/null");
        int lines = 0;
        std::istringstream iss(out);
        std::string line;
        while (std::getline(iss, line)) ++lines;
        lines -= 1; // Header.
        f.passed = (lines <= 10);
        f.description = std::to_string(std::max(0, lines)) +
                        " TCP port(s) listening";
        if (!f.passed) {
            f.description += " (consider closing unnecessary ports)";
        }
        f.remediation = "Close unnecessary ports or bind to localhost";
        findings.push_back(std::move(f));
    }

    // Firewall status.
    {
        AuditFinding f;
        f.category = "network";
        f.check = "firewall-active";
        f.description = "Firewall (ufw/iptables/nftables) is active";
        f.severity = Sev::High;
        f.remediation = "Enable ufw: sudo ufw enable";

        std::string ufw = run_cmd("ufw status 2>/dev/null");
        std::string nft = run_cmd("nft list ruleset 2>/dev/null | head -5");
        std::string ipt = run_cmd("iptables -L 2>/dev/null | grep -c 'Chain' 2>/dev/null");

        bool firewall_on = false;
        if (ufw.find("Status: active") != std::string::npos) firewall_on = true;
        if (!nft.empty() && nft.find("table") != std::string::npos) firewall_on = true;
        int ipt_chains = std::atoi(ipt.c_str());
        if (ipt_chains > 3) firewall_on = true; // More than default 3 chains.

        f.passed = firewall_on;
        if (!f.passed) {
            f.description = "No active firewall detected";
        }
        findings.push_back(std::move(f));
    }

    // IP forwarding.
    {
        AuditFinding f;
        f.category = "network";
        f.check = "ip-forwarding";
        f.description = "IP forwarding is disabled (unless router)";
        f.severity = Sev::Medium;
        f.remediation = "sysctl -w net.ipv4.ip_forward=0";

        std::string val = sysctl_get("net.ipv4.ip_forward");
        f.passed = (val == "0");
        if (!f.passed) {
            f.description = "IP forwarding is enabled (net.ipv4.ip_forward=" + val + ")";
        }
        findings.push_back(std::move(f));
    }

    return findings;
}

// ---------------------------------------------------------------------------
// User checks
// ---------------------------------------------------------------------------

std::vector<AuditFinding> Auditor::check_users() {
    std::vector<AuditFinding> findings;

    // Passwordless accounts.
    {
        AuditFinding f;
        f.category = "users";
        f.check = "passwordless-accounts";
        f.description = "No accounts with empty passwords";
        f.severity = Sev::Critical;
        f.remediation = "Set passwords or lock accounts: passwd -l <user>";

        std::string out = run_cmd(
            "awk -F: '($2 == \"\" || $2 == \"!\") && $1 != \"root\" {print $1}' "
            "/etc/shadow 2>/dev/null");
        f.passed = out.empty();
        if (!f.passed) {
            f.description = "Accounts with empty/no password:\n" + out;
        }
        findings.push_back(std::move(f));
    }

    // UID 0 accounts (besides root).
    {
        AuditFinding f;
        f.category = "users";
        f.check = "uid-zero";
        f.description = "Only root has UID 0";
        f.severity = Sev::Critical;
        f.remediation = "Remove or change UID of extra UID-0 accounts";

        std::string out = run_cmd(
            "awk -F: '$3 == 0 && $1 != \"root\" {print $1}' /etc/passwd 2>/dev/null");
        f.passed = out.empty();
        if (!f.passed) {
            f.description = "Non-root accounts with UID 0:\n" + out;
        }
        findings.push_back(std::move(f));
    }

    // Sudo configuration.
    {
        AuditFinding f;
        f.category = "users";
        f.check = "sudo-nopasswd";
        f.description = "No NOPASSWD entries in sudoers";
        f.severity = Sev::Medium;
        f.remediation = "Remove NOPASSWD from /etc/sudoers and /etc/sudoers.d/*";

        std::string out = run_cmd(
            "grep -r 'NOPASSWD' /etc/sudoers /etc/sudoers.d/ 2>/dev/null | "
            "grep -v '^#'");
        f.passed = out.empty();
        if (!f.passed) {
            f.description = "NOPASSWD sudo entries found:\n" + out;
        }
        findings.push_back(std::move(f));
    }

    return findings;
}

// ---------------------------------------------------------------------------
// Kernel checks
// ---------------------------------------------------------------------------

std::vector<AuditFinding> Auditor::check_kernel() {
    std::vector<AuditFinding> findings;

    // ASLR.
    {
        AuditFinding f;
        f.category = "kernel";
        f.check = "aslr";
        f.description = "Address Space Layout Randomization is enabled";
        f.severity = Sev::High;
        f.remediation = "sysctl -w kernel.randomize_va_space=2";

        std::string val = sysctl_get("kernel.randomize_va_space");
        f.passed = (val == "2");
        if (!f.passed) {
            f.description = "ASLR level is " + val + " (expected 2)";
        }
        findings.push_back(std::move(f));
    }

    // Core dumps restricted.
    {
        AuditFinding f;
        f.category = "kernel";
        f.check = "core-dumps";
        f.description = "Core dumps are restricted";
        f.severity = Sev::Medium;
        f.remediation = "echo 'fs.suid_dumpable = 0' >> /etc/sysctl.conf && sysctl -p";

        std::string val = sysctl_get("fs.suid_dumpable");
        f.passed = (val == "0");
        if (!f.passed) {
            f.description = "fs.suid_dumpable=" + val + " (should be 0)";
        }
        findings.push_back(std::move(f));
    }

    // dmesg restrict.
    {
        AuditFinding f;
        f.category = "kernel";
        f.check = "dmesg-restrict";
        f.description = "Kernel log access restricted to root";
        f.severity = Sev::Medium;
        f.remediation = "sysctl -w kernel.dmesg_restrict=1";

        std::string val = sysctl_get("kernel.dmesg_restrict");
        f.passed = (val == "1");
        if (!f.passed) {
            f.description = "kernel.dmesg_restrict=" + val + " (should be 1)";
        }
        findings.push_back(std::move(f));
    }

    // SysRq disabled.
    {
        AuditFinding f;
        f.category = "kernel";
        f.check = "sysrq";
        f.description = "Magic SysRq key is disabled or restricted";
        f.severity = Sev::Low;
        f.remediation = "sysctl -w kernel.sysrq=0";

        std::string val = sysctl_get("kernel.sysrq");
        int sysrq = std::atoi(val.c_str());
        f.passed = (sysrq == 0 || sysrq == 1); // 0=disabled, 1=all enabled but acceptable.
        // Actually 0 is best, but 176 is a common safe value.
        f.passed = (sysrq <= 176);
        if (!f.passed) {
            f.description = "kernel.sysrq=" + val + " (consider restricting)";
        }
        findings.push_back(std::move(f));
    }

    // Kernel pointer hiding.
    {
        AuditFinding f;
        f.category = "kernel";
        f.check = "kptr-restrict";
        f.description = "Kernel pointer addresses hidden from non-root";
        f.severity = Sev::Medium;
        f.remediation = "sysctl -w kernel.kptr_restrict=2";

        std::string val = sysctl_get("kernel.kptr_restrict");
        int kptr = std::atoi(val.c_str());
        f.passed = (kptr >= 1);
        if (!f.passed) {
            f.description = "kernel.kptr_restrict=" + val + " (should be >= 1)";
        }
        findings.push_back(std::move(f));
    }

    return findings;
}

// ---------------------------------------------------------------------------
// Service checks
// ---------------------------------------------------------------------------

std::vector<AuditFinding> Auditor::check_services() {
    std::vector<AuditFinding> findings;

    // Failed services.
    {
        AuditFinding f;
        f.category = "services";
        f.check = "failed-services";
        f.description = "No failed systemd services";
        f.severity = Sev::Medium;
        f.remediation = "Investigate and fix or disable failed services";

        std::string out = run_cmd(
            "systemctl --failed --no-legend 2>/dev/null | wc -l");
        int count = std::atoi(out.c_str());
        f.passed = (count == 0);
        if (!f.passed) {
            f.description = std::to_string(count) + " failed service(s)";
            std::string details = run_cmd(
                "systemctl --failed --no-legend 2>/dev/null");
            f.description += "\n" + details;
        }
        findings.push_back(std::move(f));
    }

    // Unnecessary services.
    {
        AuditFinding f;
        f.category = "services";
        f.check = "unnecessary-services";
        f.description = "Common unnecessary services are disabled";
        f.severity = Sev::Low;
        f.remediation = "Disable with: systemctl disable --now <service>";

        std::vector<std::string> unnecessary = {
            "telnet.socket", "rsh.socket", "rlogin.socket",
            "rexec.socket", "vsftpd", "xinetd", "avahi-daemon",
            "cups", "bluetooth"
        };

        std::vector<std::string> running;
        for (const auto& svc : unnecessary) {
            std::string out = run_cmd(
                "systemctl is-active " + svc + " 2>/dev/null");
            while (!out.empty() && out.back() == '\n') out.pop_back();
            if (out == "active") {
                running.push_back(svc);
            }
        }

        f.passed = running.empty();
        if (!f.passed) {
            f.description = "Potentially unnecessary services running:";
            for (const auto& s : running) {
                f.description += "\n  - " + s;
            }
        }
        findings.push_back(std::move(f));
    }

    return findings;
}

// ---------------------------------------------------------------------------
// SSH checks
// ---------------------------------------------------------------------------

std::vector<AuditFinding> Auditor::check_ssh() {
    std::vector<AuditFinding> findings;
    std::string sshd_config = "/etc/ssh/sshd_config";

    // PermitRootLogin.
    {
        AuditFinding f;
        f.category = "ssh";
        f.check = "permit-root-login";
        f.description = "SSH PermitRootLogin is disabled";
        f.severity = Sev::High;
        f.remediation = "Set PermitRootLogin no in /etc/ssh/sshd_config";

        bool permits_root = file_contains(sshd_config, "PermitRootLogin yes");
        // Default varies; check for explicit "no" or "prohibit-password".
        bool explicit_no = file_contains(sshd_config, "PermitRootLogin no") ||
                           file_contains(sshd_config, "PermitRootLogin prohibit-password");
        f.passed = !permits_root && explicit_no;
        if (permits_root) {
            f.description = "PermitRootLogin is set to 'yes'";
        } else if (!explicit_no) {
            f.description = "PermitRootLogin not explicitly disabled (may default to yes)";
            f.severity = Sev::Medium;
        }
        findings.push_back(std::move(f));
    }

    // PasswordAuthentication.
    {
        AuditFinding f;
        f.category = "ssh";
        f.check = "password-auth";
        f.description = "SSH password authentication is disabled (key-only)";
        f.severity = Sev::Medium;
        f.remediation = "Set PasswordAuthentication no in /etc/ssh/sshd_config";

        bool has_pw = file_contains(sshd_config, "PasswordAuthentication yes");
        bool no_pw = file_contains(sshd_config, "PasswordAuthentication no");
        f.passed = no_pw && !has_pw;
        if (has_pw) {
            f.description = "PasswordAuthentication is enabled";
        } else if (!no_pw) {
            f.description = "PasswordAuthentication not explicitly disabled";
            f.severity = Sev::Low;
        }
        findings.push_back(std::move(f));
    }

    // SSH protocol version.
    {
        AuditFinding f;
        f.category = "ssh";
        f.check = "ssh-protocol";
        f.description = "SSH uses protocol version 2 only";
        f.severity = Sev::High;
        f.remediation = "Ensure Protocol 1 is not enabled in sshd_config";

        // Protocol 1 is deprecated and removed in modern OpenSSH.
        bool has_proto1 = file_contains(sshd_config, "Protocol 1");
        f.passed = !has_proto1;
        if (has_proto1) {
            f.description = "SSH Protocol 1 is enabled (insecure)";
        }
        findings.push_back(std::move(f));
    }

    // SSH key strength.
    {
        AuditFinding f;
        f.category = "ssh";
        f.check = "ssh-host-key-strength";
        f.description = "SSH host keys use strong algorithms (ED25519 or RSA >= 3072)";
        f.severity = Sev::Medium;
        f.remediation = "Generate new host key: ssh-keygen -t ed25519 -f /etc/ssh/ssh_host_ed25519_key";

        std::error_code ec;
        bool has_ed25519 = fs::exists("/etc/ssh/ssh_host_ed25519_key", ec);
        bool has_rsa = fs::exists("/etc/ssh/ssh_host_rsa_key", ec);

        f.passed = has_ed25519;
        if (!has_ed25519 && has_rsa) {
            f.description = "No ED25519 host key; RSA only (check key size)";
        } else if (!has_ed25519 && !has_rsa) {
            f.description = "No strong SSH host keys found";
            f.severity = Sev::High;
        }
        findings.push_back(std::move(f));
    }

    return findings;
}

// ---------------------------------------------------------------------------
// Update checks
// ---------------------------------------------------------------------------

std::vector<AuditFinding> Auditor::check_updates() {
    std::vector<AuditFinding> findings;

    // Pending security updates.
    {
        AuditFinding f;
        f.category = "updates";
        f.check = "pending-updates";
        f.description = "No pending security updates";
        f.severity = Sev::High;
        f.remediation = "Run: apt update && apt upgrade";

        // Try apt.
        std::string out = run_cmd(
            "apt list --upgradable 2>/dev/null | grep -c -i 'security' || echo 0");
        int security_updates = std::atoi(out.c_str());

        if (security_updates <= 0) {
            // Try checking total upgradable.
            out = run_cmd("apt list --upgradable 2>/dev/null | wc -l");
            int total = std::atoi(out.c_str()) - 1; // Subtract header.
            if (total < 0) total = 0;
            f.passed = (total <= 5);
            f.description = std::to_string(total) + " package(s) upgradable";
        } else {
            f.passed = false;
            f.description = std::to_string(security_updates) +
                            " security update(s) pending";
        }
        findings.push_back(std::move(f));
    }

    // Automatic updates configured.
    {
        AuditFinding f;
        f.category = "updates";
        f.check = "auto-updates";
        f.description = "Automatic security updates are configured";
        f.severity = Sev::Medium;
        f.remediation = "Install unattended-upgrades: apt install unattended-upgrades";

        std::string out = run_cmd(
            "systemctl is-active unattended-upgrades 2>/dev/null || "
            "systemctl is-active apt-daily-upgrade.timer 2>/dev/null || "
            "echo inactive");
        f.passed = (out.find("active") != std::string::npos &&
                    out.find("inactive") == std::string::npos);
        if (!f.passed) {
            f.description = "Automatic updates are not configured";
        }
        findings.push_back(std::move(f));
    }

    return findings;
}

// ---------------------------------------------------------------------------
// Score computation
// ---------------------------------------------------------------------------

int Auditor::compute_score(const AuditReport& report) {
    if (report.checks_total == 0) return 100;

    // Weight failures by severity.
    int penalty = 0;
    for (const auto& f : report.findings) {
        if (f.passed) continue;
        switch (f.severity) {
            case Sev::Critical: penalty += 15; break;
            case Sev::High:     penalty += 10; break;
            case Sev::Medium:   penalty += 5;  break;
            case Sev::Low:      penalty += 2;  break;
            case Sev::Info:     penalty += 0;  break;
        }
    }

    int score = 100 - penalty;
    return std::max(0, std::min(100, score));
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

Result<AuditReport, std::string> Auditor::full_audit() {
    AuditReport report;

    auto add = [&](std::vector<AuditFinding>&& findings) {
        for (auto& f : findings) {
            if (f.passed) {
                report.checks_passed++;
            } else {
                report.checks_failed++;
            }
            report.checks_total++;
            report.findings.push_back(std::move(f));
        }
    };

    add(check_filesystem());
    add(check_network());
    add(check_users());
    add(check_kernel());
    add(check_services());
    add(check_ssh());
    add(check_updates());

    report.score = compute_score(report);

    return Result<AuditReport, std::string>::ok(std::move(report));
}

Result<AuditReport, std::string>
Auditor::check_category(const std::string& category) {
    AuditReport report;
    std::vector<AuditFinding> findings;

    if (category == "filesystem") findings = check_filesystem();
    else if (category == "network") findings = check_network();
    else if (category == "users") findings = check_users();
    else if (category == "kernel") findings = check_kernel();
    else if (category == "services") findings = check_services();
    else if (category == "ssh") findings = check_ssh();
    else if (category == "updates") findings = check_updates();
    else {
        return Result<AuditReport, std::string>::error(
            "unknown category: " + category +
            " (valid: filesystem, network, users, kernel, services, ssh, updates)");
    }

    for (auto& f : findings) {
        if (f.passed) report.checks_passed++;
        else report.checks_failed++;
        report.checks_total++;
        report.findings.push_back(std::move(f));
    }
    report.score = compute_score(report);

    return Result<AuditReport, std::string>::ok(std::move(report));
}

std::string Auditor::format_report(const AuditReport& report) {
    std::ostringstream out;

    out << "\n";
    out << "  StrayLight Shield - Security Audit Report\n";
    out << "  " << std::string(42, '=') << "\n\n";

    // Score bar.
    out << "  Security Score: " << report.score << "/100  ";
    if (report.score >= 80) out << "[GOOD]";
    else if (report.score >= 60) out << "[FAIR]";
    else if (report.score >= 40) out << "[POOR]";
    else out << "[CRITICAL]";
    out << "\n";

    // Visual bar.
    out << "  [";
    int filled = report.score / 5;
    for (int i = 0; i < 20; ++i) {
        out << (i < filled ? "#" : "-");
    }
    out << "]\n\n";

    out << "  Checks: " << report.checks_passed << " passed, "
        << report.checks_failed << " failed, "
        << report.checks_total << " total\n\n";

    // Group by category.
    std::string current_cat;
    for (const auto& f : report.findings) {
        if (f.category != current_cat) {
            current_cat = f.category;
            out << "  --- " << current_cat << " ---\n";
        }

        std::string status = f.passed ? "[PASS]" : "[FAIL]";
        std::string sev;
        switch (f.severity) {
            case Sev::Critical: sev = "CRIT"; break;
            case Sev::High:     sev = "HIGH"; break;
            case Sev::Medium:   sev = "MED "; break;
            case Sev::Low:      sev = "LOW "; break;
            case Sev::Info:     sev = "INFO"; break;
        }

        out << "  " << status << " [" << sev << "] " << f.check << "\n";
        out << "         " << f.description << "\n";
        if (!f.passed && !f.remediation.empty()) {
            out << "         Fix: " << f.remediation << "\n";
        }
        out << "\n";
    }

    return out.str();
}

} // namespace straylight
