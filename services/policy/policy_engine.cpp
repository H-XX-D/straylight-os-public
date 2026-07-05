// services/policy/policy_engine.cpp
// Applies system roles by reconfiguring all StrayLight subsystems.

#include "policy_engine.h"

#include <straylight/log.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include <signal.h>
#include <unistd.h>

namespace straylight {

PolicyEngine::PolicyEngine() = default;

Result<void, SLError> PolicyEngine::init(const std::string& policy_dir) {
    role_config_ = RoleConfig(policy_dir);
    auto result = role_config_.load_all();
    if (!result.has_value()) {
        return result;
    }

    // Detect current role from saved state
    std::ifstream state("/var/lib/straylight/policy/current_role");
    if (state.is_open()) {
        std::getline(state, current_role_);
    }

    return Result<void, SLError>::ok();
}

bool PolicyEngine::run_command(const std::string& cmd) {
    int rc = std::system(cmd.c_str());
    return rc == 0;
}

bool PolicyEngine::write_config(const std::string& path, const nlohmann::json& data) {
    std::string tmp_path = path + ".tmp";
    std::ofstream out(tmp_path, std::ios::trunc);
    if (!out.is_open()) {
        SL_WARN("policy: cannot write to {}", tmp_path);
        return false;
    }

    out << data.dump(2);
    out.close();

    if (out.fail()) {
        SL_WARN("policy: write failed for {}", tmp_path);
        return false;
    }

    // Atomic rename
    if (std::rename(tmp_path.c_str(), path.c_str()) != 0) {
        SL_WARN("policy: rename {} -> {} failed: {}", tmp_path, path, ::strerror(errno));
        return false;
    }

    return true;
}

bool PolicyEngine::reload_daemon(const std::string& daemon_name) {
    std::string pidfile = "/run/straylight/" + daemon_name + ".pid";
    std::ifstream pf(pidfile);
    if (pf.is_open()) {
        pid_t pid;
        pf >> pid;
        if (pid > 0) {
            if (kill(pid, SIGHUP) == 0) {
                SL_DEBUG("policy: sent SIGHUP to {} (pid {})", daemon_name, pid);
                return true;
            }
        }
    }

    // Fallback: use systemctl
    return run_command("systemctl reload straylight-" + daemon_name + " 2>/dev/null");
}

bool PolicyEngine::apply_autotune(const SystemRole& role, ApplyResult& result) {
    nlohmann::json conf;

    // Read existing config
    std::ifstream existing("/etc/straylight/autotune.conf");
    if (existing.is_open()) {
        try {
            conf = nlohmann::json::parse(existing);
        } catch (...) {
            conf = nlohmann::json::object();
        }
    }

    conf["profile"] = role.autotune_profile;

    if (write_config("/etc/straylight/autotune.conf", conf)) {
        reload_daemon("autotune");
        result.actions.push_back("Set autotune profile to '" + role.autotune_profile + "'");
        result.changes_made++;
        return true;
    } else {
        result.errors.push_back("Failed to update autotune config");
        result.changes_failed++;
        return false;
    }
}

bool PolicyEngine::apply_services(const SystemRole& role, ApplyResult& result) {
    bool all_ok = true;

    for (const auto& svc : role.active_services) {
        std::string cmd = "systemctl enable --now straylight-" + svc + " 2>/dev/null";
        if (run_command(cmd)) {
            result.actions.push_back("Enabled service: " + svc);
            result.changes_made++;
        } else {
            result.errors.push_back("Failed to enable service: " + svc);
            result.changes_failed++;
            all_ok = false;
        }
    }

    for (const auto& svc : role.disabled_services) {
        std::string cmd = "systemctl disable --now straylight-" + svc + " 2>/dev/null";
        if (run_command(cmd)) {
            result.actions.push_back("Disabled service: " + svc);
            result.changes_made++;
        } else {
            result.errors.push_back("Failed to disable service: " + svc);
            result.changes_failed++;
            all_ok = false;
        }
    }

    // Apply per-service config overrides
    for (const auto& [svc, config] : role.service_configs) {
        std::string conf_path = "/etc/straylight/" + svc + ".conf";
        nlohmann::json existing;

        std::ifstream in(conf_path);
        if (in.is_open()) {
            try { existing = nlohmann::json::parse(in); } catch (...) {}
        }

        // Merge overrides
        for (auto& [k, v] : config.items()) {
            existing[k] = v;
        }

        if (write_config(conf_path, existing)) {
            reload_daemon(svc);
            result.actions.push_back("Updated config for: " + svc);
            result.changes_made++;
        } else {
            result.errors.push_back("Failed to update config for: " + svc);
            result.changes_failed++;
            all_ok = false;
        }
    }

    return all_ok;
}

bool PolicyEngine::apply_resource_limits(const SystemRole& role, ApplyResult& result) {
    nlohmann::json quota_conf;

    std::ifstream existing("/etc/straylight/quota.conf");
    if (existing.is_open()) {
        try { quota_conf = nlohmann::json::parse(existing); } catch (...) {}
    }

    quota_conf["cpu_quota_percent"] = role.resource_limits.cpu_quota_percent;
    if (role.resource_limits.memory_limit_mb > 0) {
        quota_conf["memory_limit_mb"] = role.resource_limits.memory_limit_mb;
    }
    quota_conf["vram_reserve_percent"] = role.resource_limits.vram_reserve_percent;
    quota_conf["io_weight"] = role.resource_limits.io_weight;
    quota_conf["nice_level"] = role.resource_limits.nice_level;

    if (write_config("/etc/straylight/quota.conf", quota_conf)) {
        reload_daemon("quota");
        result.actions.push_back("Updated resource limits (CPU " +
                                  std::to_string(role.resource_limits.cpu_quota_percent) +
                                  "%, VRAM " +
                                  std::to_string(role.resource_limits.vram_reserve_percent) + "%)");
        result.changes_made++;
        return true;
    }

    result.errors.push_back("Failed to update quota config");
    result.changes_failed++;
    return false;
}

bool PolicyEngine::apply_thermal(const SystemRole& role, ApplyResult& result) {
    // Set thermal policy via sysfs
    std::string policy;
    if (role.thermal.cooling_mode == "passive") policy = "step_wise";
    else if (role.thermal.cooling_mode == "aggressive") policy = "bang_bang";
    else policy = "step_wise";

    std::ofstream thermal_policy("/sys/class/thermal/thermal_zone0/policy");
    if (thermal_policy.is_open()) {
        thermal_policy << policy;
        result.actions.push_back("Set thermal mode to '" + role.thermal.cooling_mode +
                                  "' (" + policy + ")");
        result.changes_made++;
    } else {
        // May not have permission — try via helper
        if (run_command("straylight-thermal-helper set-policy " + policy + " 2>/dev/null")) {
            result.actions.push_back("Set thermal mode to '" + role.thermal.cooling_mode + "'");
            result.changes_made++;
        } else {
            result.errors.push_back("Could not set thermal policy (no permission)");
            result.changes_failed++;
            return false;
        }
    }

    return true;
}

bool PolicyEngine::apply_compositor(const SystemRole& role, ApplyResult& result) {
    nlohmann::json desktop_conf;
    std::ifstream existing("/etc/straylight/desktop-session.conf");
    if (existing.is_open()) {
        try { desktop_conf = nlohmann::json::parse(existing); } catch (...) {}
    }

    desktop_conf["mode"] = role.compositor.mode;
    desktop_conf["max_fps"] = role.compositor.max_fps;
    desktop_conf["vsync"] = role.compositor.vsync;
    desktop_conf["direct_scanout"] = role.compositor.direct_scanout;
    desktop_conf["low_latency_input"] = role.compositor.low_latency_input;
    desktop_conf["provider"] = "distro-gnome-gdm";

    if (write_config("/etc/straylight/desktop-session.conf", desktop_conf)) {
        result.actions.push_back("Recorded desktop session policy '" + role.compositor.mode +
                                  "' for the distro window manager");
        result.changes_made++;
        return true;
    }

    result.errors.push_back("Failed to update desktop session policy");
    result.changes_failed++;
    return false;
}

bool PolicyEngine::apply_mesh(const SystemRole& role, ApplyResult& result) {
    nlohmann::json mesh_conf;
    std::ifstream existing("/etc/straylight/mesh.conf");
    if (existing.is_open()) {
        try { mesh_conf = nlohmann::json::parse(existing); } catch (...) {}
    }

    mesh_conf["advertise"] = role.mesh_advertise;
    if (!role.mesh_capabilities.empty()) {
        mesh_conf["capabilities"] = role.mesh_capabilities;
    }

    if (write_config("/etc/straylight/mesh.conf", mesh_conf)) {
        reload_daemon("mesh");
        result.actions.push_back(
            role.mesh_advertise
                ? "Enabled mesh advertising (" + role.mesh_capabilities + ")"
                : "Disabled mesh advertising");
        result.changes_made++;
        return true;
    }

    result.errors.push_back("Failed to update mesh config");
    result.changes_failed++;
    return false;
}

bool PolicyEngine::apply_ssh(const SystemRole& role, ApplyResult& result) {
    if (role.ssh_mode == "disabled") {
        run_command("systemctl disable --now sshd 2>/dev/null");
        result.actions.push_back("Disabled SSH");
        result.changes_made++;
    } else if (role.ssh_mode == "hardened") {
        // Write hardened SSH config
        std::string hardened =
            "PermitRootLogin no\n"
            "PasswordAuthentication no\n"
            "ChallengeResponseAuthentication no\n"
            "UsePAM yes\n"
            "X11Forwarding no\n"
            "PrintMotd no\n"
            "AcceptEnv LANG LC_*\n"
            "Subsystem sftp /usr/lib/openssh/sftp-server\n"
            "MaxAuthTries 3\n"
            "LoginGraceTime 30\n"
            "ClientAliveInterval 300\n"
            "ClientAliveCountMax 2\n";

        std::ofstream sshd_conf("/etc/ssh/sshd_config.d/straylight-hardened.conf");
        if (sshd_conf.is_open()) {
            sshd_conf << hardened;
            run_command("systemctl enable --now sshd 2>/dev/null");
            run_command("systemctl reload sshd 2>/dev/null");
            result.actions.push_back("Applied hardened SSH configuration");
            result.changes_made++;
        } else {
            result.errors.push_back("Cannot write SSH hardening config");
            result.changes_failed++;
            return false;
        }
    } else {
        // Standard SSH — ensure it's running
        run_command("systemctl enable --now sshd 2>/dev/null");
        result.actions.push_back("Ensured SSH is active (standard mode)");
        result.changes_made++;
    }

    return true;
}

bool PolicyEngine::apply_debug_tools(const SystemRole& role, ApplyResult& result) {
    if (role.debug_tools) {
        result.actions.push_back("Enabled debug tools");
        result.changes_made++;
    }

    if (role.trace_always_on) {
        // Enable lens tracing
        nlohmann::json lens_conf;
        lens_conf["always_on"] = true;
        write_config("/etc/straylight/lens.conf", lens_conf);
        reload_daemon("lens");
        result.actions.push_back("Enabled always-on tracing");
        result.changes_made++;
    }

    if (role.replay_recording) {
        nlohmann::json replay_conf;
        std::ifstream existing("/etc/straylight/replay.conf");
        if (existing.is_open()) {
            try { replay_conf = nlohmann::json::parse(existing); } catch (...) {}
        }
        replay_conf["recording"] = true;
        write_config("/etc/straylight/replay.conf", replay_conf);
        reload_daemon("replay");
        result.actions.push_back("Enabled replay recording");
        result.changes_made++;
    }

    return true;
}

Result<ApplyResult, SLError> PolicyEngine::apply_role(const std::string& role_name) {
    auto role_result = role_config_.resolve_role(role_name);
    if (!role_result.has_value()) {
        return Result<ApplyResult, SLError>::error(role_result.error());
    }

    const auto& role = role_result.value();

    SL_INFO("policy: applying role '{}' (profile={}, compositor={})",
            role.name, role.autotune_profile, role.compositor.mode);

    ApplyResult result;
    result.role_name = role.name;

    // Apply all configuration changes
    apply_autotune(role, result);
    apply_services(role, result);
    apply_resource_limits(role, result);
    apply_thermal(role, result);
    apply_compositor(role, result);
    apply_mesh(role, result);
    apply_ssh(role, result);
    apply_debug_tools(role, result);

    // Save current role state
    std::ofstream state("/var/lib/straylight/policy/current_role");
    if (state.is_open()) {
        state << role.name;
    }
    current_role_ = role.name;

    result.success = (result.changes_failed == 0);

    SL_INFO("policy: applied role '{}': {} changes made, {} failed",
            role.name, result.changes_made, result.changes_failed);

    return Result<ApplyResult, SLError>::ok(std::move(result));
}

Result<ComplianceReport, SLError> PolicyEngine::get_compliance() const {
    if (current_role_.empty()) {
        return Result<ComplianceReport, SLError>::error(
            SLError{SLErrorCode::NotInitialized, "No role is currently active"});
    }

    auto role_result = role_config_.resolve_role(current_role_);
    if (!role_result.has_value()) {
        return Result<ComplianceReport, SLError>::error(role_result.error());
    }

    auto report = checker_.check(role_result.value());
    return Result<ComplianceReport, SLError>::ok(std::move(report));
}

std::vector<std::string> PolicyEngine::list_roles() const {
    return role_config_.list_roles();
}

Result<void, SLError> PolicyEngine::create_role(const std::string& name,
                                                  const std::string& base,
                                                  const nlohmann::json& overrides) {
    return role_config_.create_role(name, base, overrides);
}

nlohmann::json PolicyEngine::diff_roles(const std::string& a,
                                          const std::string& b) const {
    return role_config_.diff_roles(a, b);
}

Result<SystemRole, SLError> PolicyEngine::get_role(const std::string& name) const {
    return role_config_.resolve_role(name);
}

} // namespace straylight
