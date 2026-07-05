// services/policy/role_config.h
// Role configuration parser and management.
#pragma once

#include <straylight/result.h>
#include <straylight/error.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace straylight {

/// Resource limits for a role.
struct ResourceLimits {
    int cpu_quota_percent{100};     // CPU quota (100 = all cores)
    int memory_limit_mb{0};         // Memory limit (0 = unlimited)
    int vram_reserve_percent{0};    // VRAM reservation for GPU workloads
    int io_weight{100};             // IO scheduling weight (1-1000)
    int nice_level{0};              // Default process nice level
};

/// Thermal configuration.
struct ThermalConfig {
    std::string cooling_mode{"balanced"}; // "passive", "balanced", "aggressive"
    int cpu_temp_warn{80};
    int cpu_temp_critical{95};
    int gpu_temp_warn{85};
    int gpu_temp_critical{100};
};

/// Compositor configuration for a role.
struct CompositorConfig {
    std::string mode{"desktop"};        // "desktop", "minimal", "disabled", "video"
    int max_fps{60};
    bool vsync{true};
    bool direct_scanout{false};
    bool low_latency_input{false};
};

/// A system role definition.
struct SystemRole {
    std::string name;
    std::string description;
    std::string base_role;                          // Inheritance: extend this role
    std::string autotune_profile{"balanced"};       // Autotune profile to apply
    std::vector<std::string> active_services;       // Services to enable
    std::vector<std::string> disabled_services;     // Services to disable
    std::map<std::string, nlohmann::json> service_configs;  // Per-service config overrides
    ResourceLimits resource_limits;
    ThermalConfig thermal;
    CompositorConfig compositor;
    bool mesh_advertise{true};                      // Advertise on mesh network
    std::string mesh_capabilities;                  // What to advertise (e.g., "gpu,vpu")
    bool debug_tools{false};                        // Enable debug tools
    bool trace_always_on{false};                    // Keep lens tracing active
    bool replay_recording{false};                   // Keep replay recording active
    std::string health_mode{"standard"};            // "minimal", "standard", "aggressive"
    std::string ssh_mode{"standard"};               // "disabled", "standard", "hardened"

    nlohmann::json to_json() const;
    static Result<SystemRole, std::string> from_json(const nlohmann::json& j);
};

/// Manages role definitions loaded from /etc/straylight/policies/.
class RoleConfig {
public:
    explicit RoleConfig(const std::filesystem::path& policy_dir =
                        "/etc/straylight/policies");

    /// Load all role definitions from the policy directory.
    Result<void, SLError> load_all();

    /// Load a single role by name.
    Result<SystemRole, SLError> load_role(const std::string& name) const;

    /// Get a fully resolved role (with inheritance applied).
    Result<SystemRole, SLError> resolve_role(const std::string& name) const;

    /// List all available roles.
    std::vector<std::string> list_roles() const;

    /// Check if a role exists.
    bool has_role(const std::string& name) const;

    /// Create a custom role based on an existing one.
    Result<void, SLError> create_role(const std::string& name,
                                       const std::string& base_role,
                                       const nlohmann::json& overrides);

    /// Compute the diff between two roles.
    nlohmann::json diff_roles(const std::string& role_a,
                               const std::string& role_b) const;

private:
    /// Apply inheritance: merge base role with derived role.
    SystemRole apply_inheritance(const SystemRole& base,
                                  const SystemRole& derived) const;

    std::filesystem::path policy_dir_;
    std::map<std::string, SystemRole> roles_;
};

} // namespace straylight
