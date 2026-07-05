// services/policy/policy_engine.h
// Policy engine — applies and monitors system role compliance.
#pragma once

#include "role_config.h"
#include "compliance_checker.h"

#include <straylight/result.h>
#include <straylight/error.h>

#include <string>

namespace straylight {

/// Result of applying a role.
struct ApplyResult {
    std::string role_name;
    bool success{true};
    int changes_made{0};
    int changes_failed{0};
    std::vector<std::string> actions;    // What was done
    std::vector<std::string> errors;     // What failed
};

/// The policy engine: applies roles and monitors compliance.
class PolicyEngine {
public:
    PolicyEngine();

    /// Initialize with the role configuration directory.
    Result<void, SLError> init(const std::string& policy_dir);

    /// Apply a named role to the system.
    Result<ApplyResult, SLError> apply_role(const std::string& role_name);

    /// Get the currently active role.
    std::string get_current_role() const { return current_role_; }

    /// Run compliance checks against the current role.
    Result<ComplianceReport, SLError> get_compliance() const;

    /// List all available roles.
    std::vector<std::string> list_roles() const;

    /// Create a new custom role.
    Result<void, SLError> create_role(const std::string& name,
                                       const std::string& base,
                                       const nlohmann::json& overrides);

    /// Diff two roles.
    nlohmann::json diff_roles(const std::string& a, const std::string& b) const;

    /// Get role details.
    Result<SystemRole, SLError> get_role(const std::string& name) const;

private:
    /// Switch the autotune profile.
    bool apply_autotune(const SystemRole& role, ApplyResult& result);

    /// Enable/disable services.
    bool apply_services(const SystemRole& role, ApplyResult& result);

    /// Set resource limits (quotas).
    bool apply_resource_limits(const SystemRole& role, ApplyResult& result);

    /// Configure thermal thresholds.
    bool apply_thermal(const SystemRole& role, ApplyResult& result);

    /// Configure compositor mode.
    bool apply_compositor(const SystemRole& role, ApplyResult& result);

    /// Configure mesh advertising.
    bool apply_mesh(const SystemRole& role, ApplyResult& result);

    /// Configure SSH mode.
    bool apply_ssh(const SystemRole& role, ApplyResult& result);

    /// Enable/disable debug tools.
    bool apply_debug_tools(const SystemRole& role, ApplyResult& result);

    /// Run a system command and return success/failure.
    static bool run_command(const std::string& cmd);

    /// Write a JSON config file atomically.
    static bool write_config(const std::string& path, const nlohmann::json& data);

    /// Send SIGHUP to a running daemon to reload config.
    static bool reload_daemon(const std::string& daemon_name);

    RoleConfig role_config_;
    ComplianceChecker checker_;
    std::string current_role_;
};

} // namespace straylight
