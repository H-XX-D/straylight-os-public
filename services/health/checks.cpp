// services/health/checks.cpp
// Health check implementations using system APIs and command-line tools.
#include "checks.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef __APPLE__
#include <mach/mach.h>
#include <sys/sysctl.h>
#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/ps/IOPSKeys.h>
#include <CoreFoundation/CoreFoundation.h>
#endif

#include <sys/statvfs.h>
#include <unistd.h>

namespace straylight {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string HealthChecks::run_cmd(const std::string& cmd) {
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    std::string result;
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) {
        result += buf;
    }
    pclose(pipe);
    return result;
}

int HealthChecks::parse_int_at(const std::string& s, size_t pos) {
    while (pos < s.size() && !isdigit(static_cast<unsigned char>(s[pos]))) ++pos;
    if (pos >= s.size()) return -1;
    auto end = s.find_first_not_of("0123456789", pos);
    try {
        return std::stoi(s.substr(pos, end - pos));
    } catch (...) {
        return -1;
    }
}

static std::string trim_copy(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    size_t start = 0;
    while (start < value.size() &&
           std::isspace(static_cast<unsigned char>(value[start]))) {
        ++start;
    }
    if (start > 0) value.erase(0, start);
    return value;
}

static std::string first_nonempty_line(const std::string& value) {
    std::istringstream lines(value);
    std::string line;
    while (std::getline(lines, line)) {
        line = trim_copy(line);
        if (!line.empty()) return line;
    }
    return "";
}

static std::string run_cmd_quiet(const std::string& cmd) {
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    std::string result;
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) {
        result += buf;
    }
    pclose(pipe);
    return result;
}

static std::string shell_quote(const std::string& value) {
    std::string quoted = "'";
    for (char c : value) {
        if (c == '\'') quoted += "'\\''";
        else quoted += c;
    }
    quoted += "'";
    return quoted;
}

static bool is_expected_inactive_straylight_unit(const std::string& unit) {
    if (unit == "straylight-firstboot.service") return true;

#ifndef __APPLE__
    auto show = run_cmd_quiet("systemctl show " + shell_quote(unit) +
                              " -p Type -p Result -p ActiveState -p SubState "
                              "-p TriggeredBy -p Triggers -p UnitFileState "
                              "--no-pager 2>/dev/null");
    std::string type;
    std::string result;
    std::string active;
    std::string sub;
    std::string triggered_by;
    std::string triggers;
    std::string unit_file_state;
    std::istringstream lines(show);
    std::string line;
    while (std::getline(lines, line)) {
        auto pos = line.find('=');
        if (pos == std::string::npos) continue;
        auto key = line.substr(0, pos);
        auto value = line.substr(pos + 1);
        if (key == "Type") type = value;
        else if (key == "Result") result = value;
        else if (key == "ActiveState") active = value;
        else if (key == "SubState") sub = value;
        else if (key == "TriggeredBy") triggered_by = value;
        else if (key == "Triggers") triggers = value;
        else if (key == "UnitFileState") unit_file_state = value;
    }
    const bool clean_inactive =
        result == "success" && active == "inactive" && sub == "dead";
    const bool timer_backed =
        triggered_by.find(".timer") != std::string::npos ||
        triggers.find(".timer") != std::string::npos;
    const bool intentionally_disabled =
        unit_file_state == "disabled" || unit_file_state == "masked";
    return clean_inactive && (type == "oneshot" || timer_backed || intentionally_disabled);
#else
    return false;
#endif
}

static bool is_active_oneshot_straylight_unit(const std::string& unit) {
#ifndef __APPLE__
    auto show = run_cmd_quiet("systemctl show " + shell_quote(unit) +
                              " -p Type -p Result -p ActiveState -p SubState "
                              "-p TriggeredBy -p Triggers --no-pager 2>/dev/null");
    std::string type;
    std::string result;
    std::string active;
    std::string sub;
    std::string triggered_by;
    std::string triggers;
    std::istringstream lines(show);
    std::string line;
    while (std::getline(lines, line)) {
        auto pos = line.find('=');
        if (pos == std::string::npos) continue;
        auto key = line.substr(0, pos);
        auto value = line.substr(pos + 1);
        if (key == "Type") type = value;
        else if (key == "Result") result = value;
        else if (key == "ActiveState") active = value;
        else if (key == "SubState") sub = value;
        else if (key == "TriggeredBy") triggered_by = value;
        else if (key == "Triggers") triggers = value;
    }

    const bool timer_backed =
        triggered_by.find(".timer") != std::string::npos ||
        triggers.find(".timer") != std::string::npos;
    const bool running_oneshot =
        type == "oneshot" &&
        (active == "active" || active == "activating") &&
        (sub == "start" || sub == "running");
    return running_oneshot && timer_backed &&
           (result.empty() || result == "success");
#else
    (void)unit;
    return false;
#endif
}

// ---------------------------------------------------------------------------
// CPU Temperature
// ---------------------------------------------------------------------------

CheckResult HealthChecks::check_cpu_temp() {
    CheckResult cr;
    cr.name = "CPU Temperature";
    cr.weight = 1.5;

    int temp_c = -1;

#ifdef __APPLE__
    // macOS: use powermetrics or smc (requires root)
    // Fallback: osx-cpu-temp or smctemp
    auto out = run_cmd("which osx-cpu-temp >/dev/null 2>&1 && osx-cpu-temp 2>/dev/null || "
                       "sysctl -n machdep.xcpm.cpu_thermal_level 2>/dev/null");
    if (!out.empty()) {
        temp_c = parse_int_at(out, 0);
    }
#else
    // Linux: /sys/class/thermal
    std::ifstream temp_file("/sys/class/thermal/thermal_zone0/temp");
    if (temp_file.is_open()) {
        int millideg = 0;
        temp_file >> millideg;
        temp_c = millideg / 1000;
    }
#endif

    if (temp_c < 0) {
        cr.score = 80; // Can't read, assume OK-ish
        cr.status = HealthStatus::Ok;
        cr.detail = "CPU temperature unavailable (sensor not accessible)";
    } else if (temp_c < 60) {
        cr.score = 100;
        cr.status = HealthStatus::Ok;
        cr.detail = std::to_string(temp_c) + "C — normal";
    } else if (temp_c < 80) {
        cr.score = std::max(0, 100 - (temp_c - 60) * 3);
        cr.status = HealthStatus::Warn;
        cr.detail = std::to_string(temp_c) + "C — elevated";
    } else {
        cr.score = std::max(0, 100 - (temp_c - 60) * 3);
        cr.status = HealthStatus::Critical;
        cr.detail = std::to_string(temp_c) + "C — critical";
    }

    return cr;
}

// ---------------------------------------------------------------------------
// RAM Pressure
// ---------------------------------------------------------------------------

CheckResult HealthChecks::check_ram_pressure() {
    CheckResult cr;
    cr.name = "Memory";
    cr.weight = 1.5;

    double used_pct = 0.0;
    uint64_t total_mb = 0;
    uint64_t free_mb = 0;

#ifdef __APPLE__
    {
        int64_t memsize = 0;
        size_t sz = sizeof(memsize);
        sysctlbyname("hw.memsize", &memsize, &sz, nullptr, 0);
        total_mb = static_cast<uint64_t>(memsize) / (1024 * 1024);

        vm_statistics64_data_t vm_stat;
        mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
        if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                             reinterpret_cast<host_info64_t>(&vm_stat), &count) == KERN_SUCCESS) {
            uint64_t page_size = 0;
            {
                int mib[2] = {CTL_HW, HW_PAGESIZE};
                size_t psz = sizeof(page_size);
                sysctl(mib, 2, &page_size, &psz, nullptr, 0);
            }
            uint64_t free_pages = static_cast<uint64_t>(vm_stat.free_count) +
                                  static_cast<uint64_t>(vm_stat.inactive_count);
            free_mb = (free_pages * page_size) / (1024 * 1024);
        }
    }
#else
    {
        std::ifstream meminfo("/proc/meminfo");
        if (meminfo.is_open()) {
            std::string line;
            while (std::getline(meminfo, line)) {
                if (line.find("MemTotal:") == 0) {
                    std::istringstream iss(line);
                    std::string label;
                    uint64_t kb;
                    iss >> label >> kb;
                    total_mb = kb / 1024;
                } else if (line.find("MemAvailable:") == 0) {
                    std::istringstream iss(line);
                    std::string label;
                    uint64_t kb;
                    iss >> label >> kb;
                    free_mb = kb / 1024;
                }
            }
        }
    }
#endif

    if (total_mb > 0) {
        used_pct = 100.0 * (1.0 - static_cast<double>(free_mb) / static_cast<double>(total_mb));
    }

    std::ostringstream detail;
    detail << static_cast<int>(used_pct) << "% used ("
           << (total_mb - free_mb) << "/" << total_mb << " MB)";

    if (used_pct < 50.0) {
        cr.score = 100;
        cr.status = HealthStatus::Ok;
    } else if (used_pct < 85.0) {
        cr.score = static_cast<int>(100 - (used_pct - 50) * 2);
        cr.status = HealthStatus::Ok;
    } else if (used_pct < 95.0) {
        cr.score = static_cast<int>(100 - (used_pct - 50) * 2);
        cr.status = HealthStatus::Warn;
    } else {
        cr.score = std::max(0, static_cast<int>(100 - (used_pct - 50) * 2));
        cr.status = HealthStatus::Critical;
    }

    cr.detail = detail.str();
    return cr;
}

// ---------------------------------------------------------------------------
// Disk Space
// ---------------------------------------------------------------------------

CheckResult HealthChecks::check_disk_space() {
    CheckResult cr;
    cr.name = "Disk Space";
    cr.weight = 1.0;

    int worst_score = 100;
    std::string worst_mount;
    double worst_pct = 0.0;

    // Check key mount points
    std::vector<std::string> mounts = {"/", "/home", "/var", "/tmp"};
    for (const auto& mount : mounts) {
        struct statvfs stat{};
        if (statvfs(mount.c_str(), &stat) != 0) continue;

        uint64_t total = stat.f_blocks * stat.f_frsize;
        uint64_t avail = stat.f_bavail * stat.f_frsize;
        if (total == 0) continue;

        double used_pct = 100.0 * (1.0 - static_cast<double>(avail) / static_cast<double>(total));
        int score;
        if (used_pct < 50.0) {
            score = 100;
        } else if (used_pct < 90.0) {
            score = static_cast<int>(100 - (used_pct - 50) * 2);
        } else {
            score = std::max(0, static_cast<int>(100 - (used_pct - 50) * 2));
        }

        if (worst_mount.empty() || score < worst_score) {
            worst_score = score;
            worst_mount = mount;
            worst_pct = used_pct;
        }
    }

    cr.score = worst_score;
    if (worst_score >= 80) {
        cr.status = HealthStatus::Ok;
    } else if (worst_score >= 30) {
        cr.status = HealthStatus::Warn;
    } else {
        cr.status = HealthStatus::Critical;
    }

    std::ostringstream detail;
    if (worst_mount.empty()) {
        detail << "No filesystems checked";
    } else {
        detail << worst_mount << " at " << static_cast<int>(worst_pct) << "% used";
    }
    cr.detail = detail.str();

    return cr;
}

// ---------------------------------------------------------------------------
// Disk SMART
// ---------------------------------------------------------------------------

CheckResult HealthChecks::check_disk_smart() {
    CheckResult cr;
    cr.name = "Disk SMART";
    cr.weight = 2.0;

    auto out = run_cmd("smartctl -H /dev/sda 2>/dev/null || "
                       "smartctl -H /dev/nvme0n1 2>/dev/null || "
                       "smartctl -H /dev/disk0 2>/dev/null");

    if (out.empty()) {
        cr.score = 90;
        cr.status = HealthStatus::Ok;
        cr.detail = "SMART data unavailable (smartctl not installed or no access)";
    } else if (out.find("PASSED") != std::string::npos ||
               out.find("OK") != std::string::npos) {
        cr.score = 100;
        cr.status = HealthStatus::Ok;
        cr.detail = "SMART health: PASSED";
    } else if (out.find("FAILING") != std::string::npos ||
               out.find("FAILED") != std::string::npos) {
        cr.score = 0;
        cr.status = HealthStatus::Critical;
        cr.detail = "SMART health: FAILING — replace drive immediately";
    } else {
        cr.score = 70;
        cr.status = HealthStatus::Warn;
        cr.detail = "SMART health: unknown status";
    }

    return cr;
}

// ---------------------------------------------------------------------------
// GPU Temperature
// ---------------------------------------------------------------------------

CheckResult HealthChecks::check_gpu_temp() {
    CheckResult cr;
    cr.name = "GPU Temperature";
    cr.weight = 1.0;

    int temp_c = -1;

    // Try NVIDIA
    auto nvidia_out = run_cmd(
        "nvidia-smi --query-gpu=temperature.gpu --format=csv,noheader 2>/dev/null");
    if (!nvidia_out.empty()) {
        temp_c = parse_int_at(nvidia_out, 0);
    }

    // Try AMD
    if (temp_c < 0) {
        auto amd_out = run_cmd(
            "cat /sys/class/drm/card0/device/hwmon/hwmon*/temp1_input 2>/dev/null");
        if (!amd_out.empty()) {
            int millideg = parse_int_at(amd_out, 0);
            if (millideg > 1000) temp_c = millideg / 1000;
        }
    }

    if (temp_c < 0) {
        cr.score = 90;
        cr.status = HealthStatus::Ok;
        cr.detail = "GPU temperature unavailable (no discrete GPU or no access)";
    } else if (temp_c < 60) {
        cr.score = 100;
        cr.status = HealthStatus::Ok;
        cr.detail = std::to_string(temp_c) + "C — normal";
    } else if (temp_c < 85) {
        cr.score = std::max(0, 100 - (temp_c - 60) * 3);
        cr.status = HealthStatus::Warn;
        cr.detail = std::to_string(temp_c) + "C — elevated";
    } else {
        cr.score = std::max(0, 100 - (temp_c - 60) * 3);
        cr.status = HealthStatus::Critical;
        cr.detail = std::to_string(temp_c) + "C — critical";
    }

    return cr;
}

// ---------------------------------------------------------------------------
// Network
// ---------------------------------------------------------------------------

CheckResult HealthChecks::check_network() {
    CheckResult cr;
    cr.name = "Network";
    cr.weight = 1.0;

    int checks_passed = 0;
    int total_checks = 3;
    std::vector<std::string> issues;

    // Gateway
    auto gw = first_nonempty_line(run_cmd(
#ifdef __APPLE__
        "route -n get default 2>/dev/null | grep gateway | awk '{print $2}'"
#else
        "ip route show default 2>/dev/null | awk '/default/ {print $3}'"
#endif
    ));
    if (!gw.empty()) {
        // Try to reach gateway
        auto ping = run_cmd("ping -c 1 -W 2 " + shell_quote(gw) + " 2>/dev/null");
        if (ping.find("1 packets received") != std::string::npos ||
            ping.find("1 received") != std::string::npos ||
            ping.find("bytes from") != std::string::npos) {
            ++checks_passed;
        } else {
            issues.push_back("Gateway unreachable");
        }
    } else {
        issues.push_back("No default gateway");
    }

    // DNS
    auto dns = run_cmd("getent hosts google.com 2>/dev/null || "
                       "host -W 2 google.com 2>/dev/null || "
                       "nslookup google.com 2>/dev/null");
    if (!trim_copy(dns).empty() ||
        dns.find("address") != std::string::npos ||
        dns.find("Address:") != std::string::npos) {
        ++checks_passed;
    } else {
        issues.push_back("DNS resolution failed");
    }

    // Internet
    auto inet = run_cmd("ping -c 1 -W 2 1.1.1.1 2>/dev/null");
    if (inet.find("bytes from") != std::string::npos) {
        ++checks_passed;
    } else {
        issues.push_back("Internet unreachable");
    }

    cr.score = (checks_passed * 100) / total_checks;
    if (checks_passed == total_checks) {
        cr.status = HealthStatus::Ok;
        cr.detail = "All connectivity checks passed";
    } else if (checks_passed > 0) {
        cr.status = HealthStatus::Warn;
        std::ostringstream oss;
        for (size_t i = 0; i < issues.size(); ++i) {
            if (i > 0) oss << "; ";
            oss << issues[i];
        }
        cr.detail = oss.str();
    } else {
        cr.status = HealthStatus::Critical;
        cr.detail = "No network connectivity";
    }

    return cr;
}

// ---------------------------------------------------------------------------
// Services
// ---------------------------------------------------------------------------

CheckResult HealthChecks::check_services() {
    CheckResult cr;
    cr.name = "Services";
    cr.weight = 1.0;

    int healthy = 0;
    int total = 0;
    std::vector<std::string> unhealthy;

#ifndef __APPLE__
    auto units = run_cmd("systemctl list-units 'straylight-*.service' "
                         "--type=service --all --no-legend --no-pager 2>/dev/null");
    std::istringstream lines(units);
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        std::string unit;
        std::string load;
        std::string active;
        std::string sub;
        fields >> unit >> load >> active >> sub;

        if (unit == "●") {
            unit = load;
            load = active;
            active = sub;
            fields >> sub;
        }

        if (unit.empty() ||
            unit.rfind("straylight-", 0) != 0 ||
            unit.find(".service") == std::string::npos ||
            is_expected_inactive_straylight_unit(unit)) {
            continue;
        }

        ++total;
        if ((active == "active" && (sub == "running" || sub == "exited")) ||
            is_active_oneshot_straylight_unit(unit)) {
            ++healthy;
        } else {
            unhealthy.push_back(unit + "=" + active + "/" + sub);
        }
    }
#endif

    if (total == 0) {
        std::vector<std::string> expected = {
            "straylight-alice", "straylight-cron"
        };

        total = static_cast<int>(expected.size());
        for (const auto& svc : expected) {
            auto out = run_cmd("pgrep -x " + svc + " 2>/dev/null");
            if (!out.empty()) ++healthy;
            else unhealthy.push_back(svc + "=not-running");
        }
    }

    if (total == 0) {
        cr.score = 100;
        cr.status = HealthStatus::Ok;
        cr.detail = "No services to monitor";
    } else {
        cr.score = (healthy * 100) / total;
        if (healthy == total) {
            cr.status = HealthStatus::Ok;
            cr.detail = "All " + std::to_string(total) + " StrayLight units healthy";
        } else {
            cr.status = (healthy > 0) ? HealthStatus::Warn : HealthStatus::Critical;
            std::ostringstream detail;
            detail << healthy << "/" << total << " StrayLight units healthy";
            if (!unhealthy.empty()) {
                detail << ": ";
                for (size_t i = 0; i < unhealthy.size() && i < 5; ++i) {
                    if (i > 0) detail << "; ";
                    detail << unhealthy[i];
                }
                if (unhealthy.size() > 5) {
                    detail << "; +" << (unhealthy.size() - 5) << " more";
                }
            }
            cr.detail = detail.str();
        }
    }

    return cr;
}

// ---------------------------------------------------------------------------
// Filesystem Errors
// ---------------------------------------------------------------------------

CheckResult HealthChecks::check_filesystem_errors() {
    CheckResult cr;
    cr.name = "Filesystem";
    cr.weight = 1.5;

    auto out = run_cmd("dmesg 2>/dev/null | grep -ci 'ext4.*error\\|btrfs.*error\\|"
                       "xfs.*error\\|filesystem.*error\\|I/O error' 2>/dev/null || "
                       "journalctl -k --since '1 hour ago' 2>/dev/null | "
                       "grep -ci 'error' 2>/dev/null");

    int errors = 0;
    if (!out.empty()) {
        try { errors = std::stoi(out); } catch (...) {}
    }

    if (errors == 0) {
        cr.score = 100;
        cr.status = HealthStatus::Ok;
        cr.detail = "No filesystem errors detected";
    } else if (errors < 5) {
        cr.score = 70;
        cr.status = HealthStatus::Warn;
        cr.detail = std::to_string(errors) + " filesystem error(s) in kernel log";
    } else {
        cr.score = std::max(0, 100 - errors * 10);
        cr.status = HealthStatus::Critical;
        cr.detail = std::to_string(errors) + " filesystem errors — check disk health";
    }

    return cr;
}

// ---------------------------------------------------------------------------
// Security Updates
// ---------------------------------------------------------------------------

CheckResult HealthChecks::check_security_updates() {
    CheckResult cr;
    cr.name = "Updates";
    cr.weight = 0.5;

    auto out = run_cmd(
        "apt list --upgradable 2>/dev/null | wc -l || "
        "softwareupdate -l 2>/dev/null | grep -c 'recommended' || "
        "echo 0");

    int updates = 0;
    if (!out.empty()) {
        try { updates = std::stoi(out); } catch (...) {}
        if (updates > 0) --updates; // Remove header line from apt
    }

    if (updates <= 0) {
        cr.score = 100;
        cr.status = HealthStatus::Ok;
        cr.detail = "System up to date";
    } else if (updates < 10) {
        cr.score = 80;
        cr.status = HealthStatus::Ok;
        cr.detail = std::to_string(updates) + " update(s) available";
    } else if (updates < 50) {
        cr.score = 50;
        cr.status = HealthStatus::Warn;
        cr.detail = std::to_string(updates) + " updates pending";
    } else {
        cr.score = 20;
        cr.status = HealthStatus::Warn;
        cr.detail = std::to_string(updates) + " updates pending — update recommended";
    }

    return cr;
}

// ---------------------------------------------------------------------------
// Battery
// ---------------------------------------------------------------------------

CheckResult HealthChecks::check_battery() {
    CheckResult cr;
    cr.name = "Battery";
    cr.weight = 0.5;

    int battery_pct = -1;
    bool charging = false;

#ifdef __APPLE__
    CFTypeRef info = IOPSCopyPowerSourcesInfo();
    if (info) {
        CFArrayRef sources = IOPSCopyPowerSourcesList(info);
        if (sources && CFArrayGetCount(sources) > 0) {
            CFDictionaryRef ps = IOPSGetPowerSourceDescription(
                info, CFArrayGetValueAtIndex(sources, 0));
            if (ps) {
                CFNumberRef cap = static_cast<CFNumberRef>(
                    CFDictionaryGetValue(ps, CFSTR(kIOPSCurrentCapacityKey)));
                if (cap) {
                    int val = 0;
                    CFNumberGetValue(cap, kCFNumberIntType, &val);
                    battery_pct = val;
                }
                CFStringRef state = static_cast<CFStringRef>(
                    CFDictionaryGetValue(ps, CFSTR(kIOPSPowerSourceStateKey)));
                if (state && CFStringCompare(state, CFSTR(kIOPSACPowerValue), 0) == kCFCompareEqualTo) {
                    charging = true;
                }
            }
        }
        if (sources) CFRelease(sources);
        CFRelease(info);
    }
#else
    // Linux: /sys/class/power_supply
    std::ifstream cap_file("/sys/class/power_supply/BAT0/capacity");
    if (cap_file.is_open()) {
        cap_file >> battery_pct;
    }
    std::ifstream status_file("/sys/class/power_supply/BAT0/status");
    if (status_file.is_open()) {
        std::string status;
        status_file >> status;
        charging = (status == "Charging" || status == "Full");
    }
#endif

    if (battery_pct < 0) {
        cr.score = 100;
        cr.status = HealthStatus::Ok;
        cr.detail = "No battery (desktop system)";
    } else {
        cr.score = battery_pct;
        std::string state = charging ? " (charging)" : " (on battery)";

        if (battery_pct >= 50) {
            cr.status = HealthStatus::Ok;
        } else if (battery_pct >= 20) {
            cr.status = HealthStatus::Warn;
        } else {
            cr.status = HealthStatus::Critical;
        }

        cr.detail = std::to_string(battery_pct) + "%" + state;
    }

    return cr;
}

// ---------------------------------------------------------------------------
// Run all
// ---------------------------------------------------------------------------

std::vector<CheckResult> HealthChecks::run_all() {
    return {
        check_cpu_temp(),
        check_ram_pressure(),
        check_disk_space(),
        check_disk_smart(),
        check_gpu_temp(),
        check_network(),
        check_services(),
        check_filesystem_errors(),
        check_security_updates(),
        check_battery(),
    };
}

} // namespace straylight
