// tools/capsule/capsule_manifest.h
// Capsule manifest parser — resource-contract app packages for StrayLight OS.
#pragma once

#include <straylight/result.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace straylight {

/// Resource contract declared by a capsule — the OS enforces these limits.
struct ResourceContract {
    uint32_t min_ram_mb = 0;
    uint32_t min_vram_mb = 0;
    uint32_t gpu_compute_percent = 0;
    bool requires_mesh = false;
    bool requires_network = false;
    uint32_t max_disk_mb = 0;
    uint32_t min_cpu_cores = 1;
};

/// Parsed capsule manifest from capsule.json inside a .capsule archive.
struct CapsuleManifest {
    std::string name;
    std::string version;
    std::string description;
    std::string binary_path;
    std::vector<std::string> dependencies;
    ResourceContract resource_contract;
};

/// Parse and validate capsule manifests.
class CapsuleManifestParser {
public:
    /// Parse a manifest from a JSON string.
    static Result<CapsuleManifest, std::string> parse(const std::string& json_str);

    /// Parse a manifest from a JSON object.
    static Result<CapsuleManifest, std::string> parse_json(const nlohmann::json& j);

    /// Validate a parsed manifest — check required fields and value ranges.
    static Result<void, std::string> validate(const CapsuleManifest& manifest);

    /// Serialize a manifest back to JSON.
    static nlohmann::json to_json(const CapsuleManifest& manifest);
};

} // namespace straylight
