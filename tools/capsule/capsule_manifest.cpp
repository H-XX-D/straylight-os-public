// tools/capsule/capsule_manifest.cpp
#include "capsule_manifest.h"

#include <sstream>

namespace straylight {

Result<CapsuleManifest, std::string> CapsuleManifestParser::parse(const std::string& json_str) {
    try {
        auto j = nlohmann::json::parse(json_str);
        return parse_json(j);
    } catch (const nlohmann::json::parse_error& e) {
        return Result<CapsuleManifest, std::string>::error(
            std::string("JSON parse error: ") + e.what());
    }
}

Result<CapsuleManifest, std::string> CapsuleManifestParser::parse_json(const nlohmann::json& j) {
    CapsuleManifest m;

    if (!j.contains("name") || !j["name"].is_string()) {
        return Result<CapsuleManifest, std::string>::error("Missing required field: name");
    }
    m.name = j["name"].get<std::string>();

    if (!j.contains("version") || !j["version"].is_string()) {
        return Result<CapsuleManifest, std::string>::error("Missing required field: version");
    }
    m.version = j["version"].get<std::string>();

    m.description = j.value("description", "");

    if (!j.contains("binary_path") || !j["binary_path"].is_string()) {
        return Result<CapsuleManifest, std::string>::error("Missing required field: binary_path");
    }
    m.binary_path = j["binary_path"].get<std::string>();

    if (j.contains("dependencies") && j["dependencies"].is_array()) {
        for (const auto& dep : j["dependencies"]) {
            if (dep.is_string()) {
                m.dependencies.push_back(dep.get<std::string>());
            }
        }
    }

    if (j.contains("resource_contract") && j["resource_contract"].is_object()) {
        const auto& rc = j["resource_contract"];
        m.resource_contract.min_ram_mb = rc.value("min_ram_mb", 0u);
        m.resource_contract.min_vram_mb = rc.value("min_vram_mb", 0u);
        m.resource_contract.gpu_compute_percent = rc.value("gpu_compute_percent", 0u);
        m.resource_contract.requires_mesh = rc.value("requires_mesh", false);
        m.resource_contract.requires_network = rc.value("requires_network", false);
        m.resource_contract.max_disk_mb = rc.value("max_disk_mb", 0u);
        m.resource_contract.min_cpu_cores = rc.value("min_cpu_cores", 1u);
    }

    auto validation = validate(m);
    if (!validation.has_value()) {
        return Result<CapsuleManifest, std::string>::error(validation.error());
    }

    return Result<CapsuleManifest, std::string>::ok(std::move(m));
}

Result<void, std::string> CapsuleManifestParser::validate(const CapsuleManifest& manifest) {
    if (manifest.name.empty()) {
        return Result<void, std::string>::error("Capsule name cannot be empty");
    }

    // Name must be alphanumeric with dashes/underscores
    for (char c : manifest.name) {
        if (!std::isalnum(c) && c != '-' && c != '_') {
            return Result<void, std::string>::error(
                "Capsule name contains invalid character: '" + std::string(1, c) +
                "' — only alphanumeric, dash, underscore allowed");
        }
    }

    if (manifest.version.empty()) {
        return Result<void, std::string>::error("Capsule version cannot be empty");
    }

    if (manifest.binary_path.empty()) {
        return Result<void, std::string>::error("Capsule binary_path cannot be empty");
    }

    // Validate resource contract ranges
    const auto& rc = manifest.resource_contract;
    if (rc.gpu_compute_percent > 100) {
        return Result<void, std::string>::error(
            "gpu_compute_percent must be 0-100, got " + std::to_string(rc.gpu_compute_percent));
    }

    if (rc.min_cpu_cores == 0) {
        return Result<void, std::string>::error("min_cpu_cores must be at least 1");
    }

    if (rc.min_cpu_cores > 256) {
        return Result<void, std::string>::error(
            "min_cpu_cores exceeds maximum (256), got " + std::to_string(rc.min_cpu_cores));
    }

    if (rc.min_ram_mb > 1024 * 1024) {
        return Result<void, std::string>::error(
            "min_ram_mb exceeds maximum (1 TiB), got " + std::to_string(rc.min_ram_mb));
    }

    if (rc.min_vram_mb > 256 * 1024) {
        return Result<void, std::string>::error(
            "min_vram_mb exceeds maximum (256 GiB), got " + std::to_string(rc.min_vram_mb));
    }

    return Result<void, std::string>::ok();
}

nlohmann::json CapsuleManifestParser::to_json(const CapsuleManifest& manifest) {
    nlohmann::json j;
    j["name"] = manifest.name;
    j["version"] = manifest.version;
    j["description"] = manifest.description;
    j["binary_path"] = manifest.binary_path;
    j["dependencies"] = manifest.dependencies;

    nlohmann::json rc;
    rc["min_ram_mb"] = manifest.resource_contract.min_ram_mb;
    rc["min_vram_mb"] = manifest.resource_contract.min_vram_mb;
    rc["gpu_compute_percent"] = manifest.resource_contract.gpu_compute_percent;
    rc["requires_mesh"] = manifest.resource_contract.requires_mesh;
    rc["requires_network"] = manifest.resource_contract.requires_network;
    rc["max_disk_mb"] = manifest.resource_contract.max_disk_mb;
    rc["min_cpu_cores"] = manifest.resource_contract.min_cpu_cores;
    j["resource_contract"] = rc;

    return j;
}

} // namespace straylight
