// tools/shield/auditor.h
// Security auditor for StrayLight OS — scans system configuration for weaknesses.
#pragma once

#include <straylight/result.h>

#include <string>
#include <vector>

namespace straylight {

/// A single audit finding.
struct AuditFinding {
    enum class Severity { Info, Low, Medium, High, Critical };

    std::string category;   // "filesystem", "network", "users", "kernel", "services", "ssh", "updates"
    std::string check;       // Short name of the check.
    std::string description; // Human-readable explanation.
    Severity severity = Severity::Info;
    bool passed = false;
    std::string remediation; // Suggested fix.
};

/// Full audit report.
struct AuditReport {
    int score = 0;           // 0-100, higher is better.
    int checks_passed = 0;
    int checks_failed = 0;
    int checks_total = 0;
    std::vector<AuditFinding> findings;
};

/// Runs security audit checks against the system.
class Auditor {
public:
    Auditor();
    ~Auditor();

    /// Run a full security audit.
    Result<AuditReport, std::string> full_audit();

    /// Run checks for a specific category.
    Result<AuditReport, std::string> check_category(const std::string& category);

    /// Generate a formatted text report.
    static std::string format_report(const AuditReport& report);

private:
    // Individual category checkers.
    std::vector<AuditFinding> check_filesystem();
    std::vector<AuditFinding> check_network();
    std::vector<AuditFinding> check_users();
    std::vector<AuditFinding> check_kernel();
    std::vector<AuditFinding> check_services();
    std::vector<AuditFinding> check_ssh();
    std::vector<AuditFinding> check_updates();

    // Helpers.
    std::string run_cmd(const std::string& cmd);
    bool file_contains(const std::string& path, const std::string& needle);
    std::string sysctl_get(const std::string& key);
    int compute_score(const AuditReport& report);
};

} // namespace straylight
