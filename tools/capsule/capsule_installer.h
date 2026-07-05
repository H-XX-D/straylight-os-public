// tools/capsule/capsule_installer.h
// Install and manage capsule packages on the system.
#pragma once

#include "capsule_manifest.h"

#include <straylight/result.h>

#include <filesystem>
#include <string>
#include <vector>

namespace straylight {

/// Information about an installed capsule.
struct InstalledCapsule {
    std::string name;
    std::string version;
    std::string description;
    std::string install_path;
    ResourceContract resource_contract;
};

/// Installs, uninstalls, and queries capsule packages.
class CapsuleInstaller {
public:
    /// Base directory for capsule installations.
    static constexpr const char* CAPSULE_BASE = "/opt/straylight/capsules";

    /// Install a .capsule archive to the system.
    static Result<InstalledCapsule, std::string> install(const std::filesystem::path& capsule_path);

    /// Uninstall a capsule by name — removes files, desktop entry, quota registration.
    static Result<void, std::string> uninstall(const std::string& name);

    /// List all installed capsules.
    static std::vector<InstalledCapsule> list_installed();

    /// Get info about a specific installed capsule.
    static Result<InstalledCapsule, std::string> get_installed(const std::string& name);

private:
    /// Verify the archive hash against its .sha256 sidecar.
    static Result<void, std::string> verify_hash(const std::filesystem::path& archive_path);

    /// Extract the archive to a temporary location and return the extracted dir.
    static Result<std::filesystem::path, std::string> extract_archive(
        const std::filesystem::path& archive_path);

    /// Check that the system can satisfy the resource contract.
    static Result<void, std::string> check_system_capabilities(const ResourceContract& contract);

    /// Query system RAM in MB.
    static uint32_t get_system_ram_mb();

    /// Query VPU VRAM in MB from sysfs.
    static uint32_t get_system_vram_mb();

    /// Query CPU core count.
    static uint32_t get_cpu_core_count();

    /// Check if mesh service is running.
    static bool is_mesh_available();

    /// Install apt dependencies.
    static Result<void, std::string> install_dependencies(
        const std::vector<std::string>& dependencies);

    /// Register capsule with the quota service (set resource limits from contract).
    static Result<void, std::string> register_with_quota(const CapsuleManifest& manifest);

    /// Unregister capsule from the quota service.
    static Result<void, std::string> unregister_from_quota(const std::string& name);

    /// Create a .desktop entry for the capsule in /usr/share/applications/.
    static Result<void, std::string> create_desktop_entry(const CapsuleManifest& manifest,
                                                           const std::filesystem::path& install_dir);

    /// Remove the .desktop entry for a capsule.
    static Result<void, std::string> remove_desktop_entry(const std::string& name);
};

} // namespace straylight
