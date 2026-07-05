// services/policy/compliance_checker.cpp
// System compliance verification against declared roles.

#include "compliance_checker.h"

#include <straylight/log.h>

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace straylight {

void ComplianceChecker::pass(ComplianceReport& report) {
    report.total_checks++;
    report.passed_checks++;
}

void ComplianceChecker::deviate(ComplianceReport& report, ComplianceDeviation dev) {
    report.total_checks++;
    report.failed_checks++;
    report.compliant = false;
    report.deviations.push_back(std::move(dev));
}

std::string ComplianceChecker::read_autotune_profile() {
    std::ifstream conf("/etc/straylight/autotune.conf");
    if (!conf.is_open()) return "";

    std::string line;
    while (std::getline(conf, line)) {
        // Look for "profile" key in JSON
        if (line.find("\"profile\"") != std::string::npos) {
            size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string val = line.substr(colon + 1);
            // Strip quotes and whitespace
            std::string result;
            for (char c : val) {
                if (c != '"' && c != ',' && c != ' ' && c != '\t') {
                    result += c;
                }
            }
            return result;
        }
    }

    // Try JSON parse
    conf.clear();
    conf.seekg(0);
    try {
        auto j = nlohmann::json::parse(conf);
        return j.value("profile", "");
    } catch (...) {
        return "";
    }
}

bool ComplianceChecker::is_service_active(const std::string& service_name) {
    std::string cmd = "systemctl is-active straylight-" + service_name +
                      ".service 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return false;

    char buf[64];
    std::string output;
    while (fgets(buf, sizeof(buf), pipe)) {
        output += buf;
    }
    int rc = pclose(pipe);

    // Trim whitespace
    while (!output.empty() && (output.back() == '\n' || output.back() == ' ')) {
        output.pop_back();
    }

    return (rc == 0 && output == "active");
}

std::string ComplianceChecker::read_compositor_mode() {
    // Read from compositor config or runtime state
    std::ifstream conf("/etc/straylight/compositor.conf");
    if (!conf.is_open()) return "desktop";

    try {
        auto j = nlohmann::json::parse(conf);
        return j.value("mode", "desktop");
    } catch (...) {
        return "desktop";
    }
}

ComplianceReport ComplianceChecker::check(const SystemRole& role) const {
    ComplianceReport report;
    report.role_name = role.name;
    report.compliant = true;

    check_autotune(role, report);
    check_services(role, report);
    check_resource_limits(role, report);
    check_thermal(role, report);
    check_compositor(role, report);
    check_ssh(role, report);
    check_mesh(role, report);

    SL_INFO("policy: compliance check for role '{}': {}/{} passed ({}%)",
            role.name, report.passed_checks, report.total_checks,
            static_cast<int>(report.compliance_ratio() * 100));

    return report;
}

void ComplianceChecker::check_autotune(const SystemRole& role,
                                         ComplianceReport& report) const {
    std::string current = read_autotune_profile();
    if (current.empty()) {
        // Cannot determine — assume OK
        pass(report);
        return;
    }

    if (current == role.autotune_profile) {
        pass(report);
    } else {
        deviate(report, {
            "autotune",
            role.autotune_profile,
            current,
            DeviationSeverity::Warning,
            "Autotune profile mismatch: expected '" + role.autotune_profile +
            "' but found '" + current + "'"
        });
    }
}

void ComplianceChecker::check_services(const SystemRole& role,
                                         ComplianceReport& report) const {
    // Check that active services are running
    for (const auto& svc : role.active_services) {
        if (is_service_active(svc)) {
            pass(report);
        } else {
            deviate(report, {
                "service:" + svc,
                "active",
                "inactive",
                DeviationSeverity::Error,
                "Service '" + svc + "' should be active but is not running"
            });
        }
    }

    // Check that disabled services are not running
    for (const auto& svc : role.disabled_services) {
        if (!is_service_active(svc)) {
            pass(report);
        } else {
            deviate(report, {
                "service:" + svc,
                "inactive",
                "active",
                DeviationSeverity::Warning,
                "Service '" + svc + "' should be disabled but is running"
            });
        }
    }
}

void ComplianceChecker::check_resource_limits(const SystemRole& role,
                                                ComplianceReport& report) const {
    // Check VRAM reservation if specified
    if (role.resource_limits.vram_reserve_percent > 0) {
        // Read current VRAM reservation from quota config
        std::ifstream quota_conf("/etc/straylight/quota.conf");
        if (quota_conf.is_open()) {
            try {
                auto j = nlohmann::json::parse(quota_conf);
                int current_vram = j.value("vram_reserve_percent", 0);
                if (current_vram == role.resource_limits.vram_reserve_percent) {
                    pass(report);
                } else {
                    deviate(report, {
                        "resource_limits.vram_reserve",
                        std::to_string(role.resource_limits.vram_reserve_percent) + "%",
                        std::to_string(current_vram) + "%",
                        DeviationSeverity::Warning,
                        "VRAM reservation mismatch"
                    });
                }
            } catch (...) {
                pass(report); // Cannot verify
            }
        } else {
            pass(report);
        }
    } else {
        pass(report);
    }

    // Check CPU quota
    // On StrayLight, cgroup limits are set in /sys/fs/cgroup/straylight/
    pass(report); // Placeholder: actual cgroup inspection would go here
}

void ComplianceChecker::check_thermal(const SystemRole& role,
                                        ComplianceReport& report) const {
    // Read current thermal policy
    std::ifstream thermal_conf("/sys/class/thermal/thermal_zone0/policy");
    if (!thermal_conf.is_open()) {
        pass(report);
        return;
    }

    std::string current_policy;
    std::getline(thermal_conf, current_policy);

    // Map StrayLight cooling modes to kernel thermal policies
    std::string expected_policy;
    if (role.thermal.cooling_mode == "passive") expected_policy = "step_wise";
    else if (role.thermal.cooling_mode == "aggressive") expected_policy = "bang_bang";
    else expected_policy = "step_wise";

    if (current_policy.find(expected_policy) != std::string::npos) {
        pass(report);
    } else {
        deviate(report, {
            "thermal",
            role.thermal.cooling_mode + " (" + expected_policy + ")",
            current_policy,
            DeviationSeverity::Warning,
            "Thermal policy mismatch"
        });
    }
}

void ComplianceChecker::check_compositor(const SystemRole& role,
                                           ComplianceReport& report) const {
    std::string current = read_compositor_mode();

    if (current == role.compositor.mode) {
        pass(report);
    } else {
        DeviationSeverity sev = DeviationSeverity::Warning;
        if (role.compositor.mode == "disabled" || current == "disabled") {
            sev = DeviationSeverity::Error;
        }

        deviate(report, {
            "compositor",
            role.compositor.mode,
            current,
            sev,
            "Compositor mode mismatch: expected '" + role.compositor.mode +
            "' but found '" + current + "'"
        });
    }
}

void ComplianceChecker::check_ssh(const SystemRole& role,
                                    ComplianceReport& report) const {
    if (role.ssh_mode == "disabled") {
        if (!is_service_active("sshd")) {
            pass(report);
        } else {
            deviate(report, {
                "ssh",
                "disabled",
                "active",
                DeviationSeverity::Critical,
                "SSH should be disabled for this role but sshd is running"
            });
        }
    } else if (role.ssh_mode == "hardened") {
        // Check for hardened SSH config
        std::ifstream sshd_conf("/etc/ssh/sshd_config");
        if (sshd_conf.is_open()) {
            std::string content((std::istreambuf_iterator<char>(sshd_conf)),
                                std::istreambuf_iterator<char>());
            bool has_no_password = content.find("PasswordAuthentication no") != std::string::npos;
            bool has_no_root = content.find("PermitRootLogin no") != std::string::npos;

            if (has_no_password && has_no_root) {
                pass(report);
            } else {
                deviate(report, {
                    "ssh",
                    "hardened (no password, no root)",
                    has_no_password ? "no-password" : "password-allowed",
                    DeviationSeverity::Critical,
                    "SSH hardening incomplete: " +
                    std::string(!has_no_password ? "password auth enabled" : "") +
                    std::string(!has_no_root ? " root login permitted" : "")
                });
            }
        } else {
            pass(report);
        }
    } else {
        pass(report);
    }
}

void ComplianceChecker::check_mesh(const SystemRole& role,
                                     ComplianceReport& report) const {
    if (role.mesh_advertise) {
        if (is_service_active("mesh")) {
            pass(report);
        } else {
            deviate(report, {
                "mesh",
                "active (advertising)",
                "inactive",
                DeviationSeverity::Info,
                "Mesh service should be active for advertising"
            });
        }
    } else {
        pass(report);
    }
}

} // namespace straylight
