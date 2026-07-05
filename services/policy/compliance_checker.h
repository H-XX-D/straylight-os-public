// services/policy/compliance_checker.h
// Verifies the system matches the declared role.
#pragma once

#include "role_config.h"

#include <straylight/result.h>
#include <straylight/error.h>

#include <string>
#include <vector>

namespace straylight {

/// Severity of a compliance deviation.
enum class DeviationSeverity : uint8_t {
    Info,       // Cosmetic difference
    Warning,    // Non-critical deviation
    Error,      // Service in wrong state
    Critical,   // Security-relevant deviation
};

inline const char* deviation_severity_name(DeviationSeverity s) {
    switch (s) {
        case DeviationSeverity::Info:     return "info";
        case DeviationSeverity::Warning:  return "warning";
        case DeviationSeverity::Error:    return "error";
        case DeviationSeverity::Critical: return "critical";
    }
    return "unknown";
}

/// A single compliance deviation.
struct ComplianceDeviation {
    std::string component;          // e.g., "autotune", "compositor", "service:health"
    std::string expected;           // What the role says
    std::string actual;             // What the system has
    DeviationSeverity severity{DeviationSeverity::Warning};
    std::string description;        // Human-readable explanation
};

/// Overall compliance report.
struct ComplianceReport {
    std::string role_name;
    bool compliant{true};
    int total_checks{0};
    int passed_checks{0};
    int failed_checks{0};
    std::vector<ComplianceDeviation> deviations;

    [[nodiscard]] double compliance_ratio() const {
        return (total_checks > 0)
            ? static_cast<double>(passed_checks) / static_cast<double>(total_checks)
            : 1.0;
    }
};

/// Checks whether the running system matches the declared role.
class ComplianceChecker {
public:
    /// Run all compliance checks against the given role.
    ComplianceReport check(const SystemRole& role) const;

private:
    /// Check if the autotune profile matches.
    void check_autotune(const SystemRole& role, ComplianceReport& report) const;

    /// Check if required services are running and disabled ones are stopped.
    void check_services(const SystemRole& role, ComplianceReport& report) const;

    /// Check resource limits (quotas).
    void check_resource_limits(const SystemRole& role, ComplianceReport& report) const;

    /// Check thermal configuration.
    void check_thermal(const SystemRole& role, ComplianceReport& report) const;

    /// Check compositor mode.
    void check_compositor(const SystemRole& role, ComplianceReport& report) const;

    /// Check SSH configuration.
    void check_ssh(const SystemRole& role, ComplianceReport& report) const;

    /// Check mesh advertising.
    void check_mesh(const SystemRole& role, ComplianceReport& report) const;

    /// Read the current autotune profile from the config file.
    static std::string read_autotune_profile();

    /// Check if a systemd service is active.
    static bool is_service_active(const std::string& service_name);

    /// Read current compositor mode.
    static std::string read_compositor_mode();

    /// Add a passed check to the report.
    static void pass(ComplianceReport& report);

    /// Add a deviation to the report.
    static void deviate(ComplianceReport& report, ComplianceDeviation dev);
};

} // namespace straylight
