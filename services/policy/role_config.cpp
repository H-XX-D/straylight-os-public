// services/policy/role_config.cpp
// Role configuration loading, inheritance resolution, and management.

#include "role_config.h"

#include <straylight/log.h>

#include <fstream>
#include <set>

namespace straylight {

nlohmann::json SystemRole::to_json() const {
    nlohmann::json j;
    j["name"] = name;
    j["description"] = description;
    if (!base_role.empty()) j["base_role"] = base_role;
    j["autotune_profile"] = autotune_profile;
    j["active_services"] = active_services;
    j["disabled_services"] = disabled_services;
    j["service_configs"] = service_configs;

    j["resource_limits"]["cpu_quota_percent"] = resource_limits.cpu_quota_percent;
    j["resource_limits"]["memory_limit_mb"] = resource_limits.memory_limit_mb;
    j["resource_limits"]["vram_reserve_percent"] = resource_limits.vram_reserve_percent;
    j["resource_limits"]["io_weight"] = resource_limits.io_weight;
    j["resource_limits"]["nice_level"] = resource_limits.nice_level;

    j["thermal"]["cooling_mode"] = thermal.cooling_mode;
    j["thermal"]["cpu_temp_warn"] = thermal.cpu_temp_warn;
    j["thermal"]["cpu_temp_critical"] = thermal.cpu_temp_critical;
    j["thermal"]["gpu_temp_warn"] = thermal.gpu_temp_warn;
    j["thermal"]["gpu_temp_critical"] = thermal.gpu_temp_critical;

    j["compositor"]["mode"] = compositor.mode;
    j["compositor"]["max_fps"] = compositor.max_fps;
    j["compositor"]["vsync"] = compositor.vsync;
    j["compositor"]["direct_scanout"] = compositor.direct_scanout;
    j["compositor"]["low_latency_input"] = compositor.low_latency_input;

    j["mesh_advertise"] = mesh_advertise;
    j["mesh_capabilities"] = mesh_capabilities;
    j["debug_tools"] = debug_tools;
    j["trace_always_on"] = trace_always_on;
    j["replay_recording"] = replay_recording;
    j["health_mode"] = health_mode;
    j["ssh_mode"] = ssh_mode;

    return j;
}

Result<SystemRole, std::string> SystemRole::from_json(const nlohmann::json& j) {
    SystemRole role;

    role.name = j.value("name", "");
    role.description = j.value("description", "");
    role.base_role = j.value("base_role", "");
    role.autotune_profile = j.value("autotune_profile", "balanced");

    if (j.contains("active_services") && j["active_services"].is_array()) {
        for (const auto& s : j["active_services"]) {
            role.active_services.push_back(s.get<std::string>());
        }
    }

    if (j.contains("disabled_services") && j["disabled_services"].is_array()) {
        for (const auto& s : j["disabled_services"]) {
            role.disabled_services.push_back(s.get<std::string>());
        }
    }

    if (j.contains("service_configs") && j["service_configs"].is_object()) {
        for (auto& [k, v] : j["service_configs"].items()) {
            role.service_configs[k] = v;
        }
    }

    if (j.contains("resource_limits")) {
        const auto& rl = j["resource_limits"];
        role.resource_limits.cpu_quota_percent = rl.value("cpu_quota_percent", 100);
        role.resource_limits.memory_limit_mb = rl.value("memory_limit_mb", 0);
        role.resource_limits.vram_reserve_percent = rl.value("vram_reserve_percent", 0);
        role.resource_limits.io_weight = rl.value("io_weight", 100);
        role.resource_limits.nice_level = rl.value("nice_level", 0);
    }

    if (j.contains("thermal")) {
        const auto& th = j["thermal"];
        role.thermal.cooling_mode = th.value("cooling_mode", "balanced");
        role.thermal.cpu_temp_warn = th.value("cpu_temp_warn", 80);
        role.thermal.cpu_temp_critical = th.value("cpu_temp_critical", 95);
        role.thermal.gpu_temp_warn = th.value("gpu_temp_warn", 85);
        role.thermal.gpu_temp_critical = th.value("gpu_temp_critical", 100);
    }

    if (j.contains("compositor")) {
        const auto& cm = j["compositor"];
        role.compositor.mode = cm.value("mode", "desktop");
        role.compositor.max_fps = cm.value("max_fps", 60);
        role.compositor.vsync = cm.value("vsync", true);
        role.compositor.direct_scanout = cm.value("direct_scanout", false);
        role.compositor.low_latency_input = cm.value("low_latency_input", false);
    }

    role.mesh_advertise = j.value("mesh_advertise", true);
    role.mesh_capabilities = j.value("mesh_capabilities", "");
    role.debug_tools = j.value("debug_tools", false);
    role.trace_always_on = j.value("trace_always_on", false);
    role.replay_recording = j.value("replay_recording", false);
    role.health_mode = j.value("health_mode", "standard");
    role.ssh_mode = j.value("ssh_mode", "standard");

    return Result<SystemRole, std::string>::ok(std::move(role));
}

RoleConfig::RoleConfig(const std::filesystem::path& policy_dir)
    : policy_dir_(policy_dir) {}

Result<void, SLError> RoleConfig::load_all() {
    std::error_code ec;
    if (!std::filesystem::exists(policy_dir_, ec)) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::NotFound,
                    "Policy directory not found: " + policy_dir_.string()});
    }

    roles_.clear();
    int loaded = 0;

    for (const auto& entry : std::filesystem::directory_iterator(policy_dir_, ec)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".json") continue;

        std::ifstream file(entry.path());
        if (!file.is_open()) continue;

        try {
            auto j = nlohmann::json::parse(file);
            auto result = SystemRole::from_json(j);
            if (result.has_value()) {
                auto role = std::move(result.value());
                if (role.name.empty()) {
                    role.name = entry.path().stem().string();
                }
                roles_[role.name] = std::move(role);
                ++loaded;
            }
        } catch (const nlohmann::json::exception& e) {
            SL_WARN("policy: failed to parse {}: {}",
                    entry.path().string(), e.what());
        }
    }

    SL_INFO("policy: loaded {} role definitions from {}", loaded, policy_dir_.string());
    return Result<void, SLError>::ok();
}

Result<SystemRole, SLError> RoleConfig::load_role(const std::string& name) const {
    auto it = roles_.find(name);
    if (it == roles_.end()) {
        return Result<SystemRole, SLError>::error(
            SLError{SLErrorCode::NotFound, "Role not found: " + name});
    }
    return Result<SystemRole, SLError>::ok(it->second);
}

SystemRole RoleConfig::apply_inheritance(const SystemRole& base,
                                           const SystemRole& derived) const {
    SystemRole merged = base;

    // Override with derived values
    merged.name = derived.name;
    merged.description = derived.description.empty() ? base.description : derived.description;

    if (derived.autotune_profile != "balanced" || base.autotune_profile.empty()) {
        merged.autotune_profile = derived.autotune_profile;
    }

    // Merge service lists (derived overrides base)
    if (!derived.active_services.empty()) {
        merged.active_services = derived.active_services;
    }
    if (!derived.disabled_services.empty()) {
        merged.disabled_services = derived.disabled_services;
    }

    // Merge service configs (derived overrides per-key)
    for (const auto& [k, v] : derived.service_configs) {
        merged.service_configs[k] = v;
    }

    // Override resource limits if explicitly set
    if (derived.resource_limits.cpu_quota_percent != 100) {
        merged.resource_limits.cpu_quota_percent = derived.resource_limits.cpu_quota_percent;
    }
    if (derived.resource_limits.memory_limit_mb != 0) {
        merged.resource_limits.memory_limit_mb = derived.resource_limits.memory_limit_mb;
    }
    if (derived.resource_limits.vram_reserve_percent != 0) {
        merged.resource_limits.vram_reserve_percent = derived.resource_limits.vram_reserve_percent;
    }
    if (derived.resource_limits.io_weight != 100) {
        merged.resource_limits.io_weight = derived.resource_limits.io_weight;
    }
    if (derived.resource_limits.nice_level != 0) {
        merged.resource_limits.nice_level = derived.resource_limits.nice_level;
    }

    // Override thermal
    if (derived.thermal.cooling_mode != "balanced") {
        merged.thermal = derived.thermal;
    }

    // Override compositor
    if (derived.compositor.mode != "desktop") {
        merged.compositor = derived.compositor;
    }

    // Override flags
    merged.mesh_advertise = derived.mesh_advertise;
    if (!derived.mesh_capabilities.empty()) {
        merged.mesh_capabilities = derived.mesh_capabilities;
    }
    merged.debug_tools = derived.debug_tools;
    merged.trace_always_on = derived.trace_always_on;
    merged.replay_recording = derived.replay_recording;
    if (derived.health_mode != "standard") merged.health_mode = derived.health_mode;
    if (derived.ssh_mode != "standard") merged.ssh_mode = derived.ssh_mode;

    return merged;
}

Result<SystemRole, SLError> RoleConfig::resolve_role(const std::string& name) const {
    auto it = roles_.find(name);
    if (it == roles_.end()) {
        return Result<SystemRole, SLError>::error(
            SLError{SLErrorCode::NotFound, "Role not found: " + name});
    }

    SystemRole resolved = it->second;

    // Walk up the inheritance chain
    std::string base_name = resolved.base_role;
    int depth = 0;
    while (!base_name.empty() && depth < 10) {
        auto base_it = roles_.find(base_name);
        if (base_it == roles_.end()) break;

        resolved = apply_inheritance(base_it->second, resolved);
        base_name = base_it->second.base_role;
        ++depth;
    }

    return Result<SystemRole, SLError>::ok(std::move(resolved));
}

std::vector<std::string> RoleConfig::list_roles() const {
    std::vector<std::string> names;
    names.reserve(roles_.size());
    for (const auto& [name, _] : roles_) {
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

bool RoleConfig::has_role(const std::string& name) const {
    return roles_.count(name) > 0;
}

Result<void, SLError> RoleConfig::create_role(const std::string& name,
                                                const std::string& base_role,
                                                const nlohmann::json& overrides) {
    if (roles_.count(name) > 0) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::AlreadyExists, "Role already exists: " + name});
    }

    if (!base_role.empty() && roles_.count(base_role) == 0) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::NotFound, "Base role not found: " + base_role});
    }

    // Start with base role JSON, merge overrides
    nlohmann::json role_json;
    if (!base_role.empty()) {
        auto base_it = roles_.find(base_role);
        if (base_it != roles_.end()) {
            role_json = base_it->second.to_json();
        }
    }

    role_json["name"] = name;
    role_json["base_role"] = base_role;

    // Apply overrides
    for (auto& [k, v] : overrides.items()) {
        role_json[k] = v;
    }

    // Parse and store
    auto result = SystemRole::from_json(role_json);
    if (!result.has_value()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::ParseError, result.error()});
    }

    auto role = std::move(result.value());

    // Write to disk
    std::filesystem::path path = policy_dir_ / (name + ".json");
    std::ofstream out(path);
    if (!out.is_open()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError, "Cannot write to " + path.string()});
    }
    out << role_json.dump(2);

    roles_[name] = std::move(role);

    SL_INFO("policy: created custom role '{}' based on '{}'", name, base_role);
    return Result<void, SLError>::ok();
}

nlohmann::json RoleConfig::diff_roles(const std::string& role_a,
                                        const std::string& role_b) const {
    nlohmann::json diff;

    auto a_result = resolve_role(role_a);
    auto b_result = resolve_role(role_b);

    if (!a_result.has_value() || !b_result.has_value()) {
        diff["error"] = "One or both roles not found";
        return diff;
    }

    auto ja = a_result.value().to_json();
    auto jb = b_result.value().to_json();

    diff["role_a"] = role_a;
    diff["role_b"] = role_b;
    nlohmann::json changes = nlohmann::json::array();

    // Compare top-level keys
    std::set<std::string> all_keys;
    for (auto& [k, _] : ja.items()) all_keys.insert(k);
    for (auto& [k, _] : jb.items()) all_keys.insert(k);

    for (const auto& key : all_keys) {
        if (key == "name" || key == "base_role") continue;

        bool in_a = ja.contains(key);
        bool in_b = jb.contains(key);

        if (in_a && in_b) {
            if (ja[key] != jb[key]) {
                nlohmann::json change;
                change["field"] = key;
                change["role_a"] = ja[key];
                change["role_b"] = jb[key];
                changes.push_back(change);
            }
        } else if (in_a) {
            nlohmann::json change;
            change["field"] = key;
            change["role_a"] = ja[key];
            change["role_b"] = nullptr;
            changes.push_back(change);
        } else {
            nlohmann::json change;
            change["field"] = key;
            change["role_a"] = nullptr;
            change["role_b"] = jb[key];
            changes.push_back(change);
        }
    }

    diff["changes"] = changes;
    diff["change_count"] = changes.size();

    return diff;
}

} // namespace straylight
