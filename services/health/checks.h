// services/health/checks.h
// Individual health check implementations for StrayLight Health daemon.
#pragma once

#include <straylight/result.h>

#include <string>
#include <vector>

namespace straylight {

/// Status level for a health check.
enum class HealthStatus { Ok, Warn, Critical };

/// Result of a single health check.
struct CheckResult {
    std::string name;
    int score = 100;           // 0-100
    HealthStatus status = HealthStatus::Ok;
    std::string detail;
    double weight = 1.0;       // Weight in overall score
};

/// Individual health check runners.
class HealthChecks {
public:
    /// CPU temperature check.
    /// Score: 100 if < 60C, degrades linearly to 0 at 100C.
    static CheckResult check_cpu_temp();

    /// RAM pressure check.
    /// Score: 100 if < 50% used, degrades linearly to 0 at 100%.
    static CheckResult check_ram_pressure();

    /// Disk space check (all mounted filesystems).
    /// Score: min across all mounts; 100 if < 50% used, 0 at 100%.
    static CheckResult check_disk_space();

    /// Disk SMART health (if smartctl available).
    /// Score: 100 if PASSED, 0 if FAILED.
    static CheckResult check_disk_smart();

    /// GPU temperature check.
    /// Score: 100 if < 60C, degrades to 0 at 100C.
    static CheckResult check_gpu_temp();

    /// Network connectivity check.
    /// Score: 100 if internet + DNS + gateway all work.
    static CheckResult check_network();

    /// StrayLight service status check.
    /// Score based on percentage of expected services running.
    static CheckResult check_services();

    /// Filesystem error check (dmesg / journal).
    /// Score: 100 if no errors, degrades with error count.
    static CheckResult check_filesystem_errors();

    /// Security updates pending check.
    /// Score: 100 if none, 50 if some, 0 if critical.
    static CheckResult check_security_updates();

    /// Battery check (laptop only).
    /// Score: maps battery percentage; warns below 20%.
    static CheckResult check_battery();

    /// Run all checks and return results.
    static std::vector<CheckResult> run_all();

private:
    /// Helper: run a command and return stdout.
    static std::string run_cmd(const std::string& cmd);

    /// Helper: parse a number from a string starting at pos.
    static int parse_int_at(const std::string& s, size_t pos);
};

} // namespace straylight
