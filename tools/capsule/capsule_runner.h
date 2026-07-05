// tools/capsule/capsule_runner.h
// Launch capsules with resource enforcement via cgroups and VPU ioctls.
#pragma once

#include "capsule_manifest.h"

#include <straylight/result.h>

#include <cstdint>
#include <string>
#include <vector>

namespace straylight {

/// Information about a running capsule process.
struct RunningCapsule {
    std::string name;
    pid_t pid;
    ResourceContract contract;
    uint32_t current_ram_mb;
    uint32_t current_vram_mb;
    float cpu_usage_percent;
    uint64_t uptime_seconds;
};

/// Launches and monitors capsule processes with resource enforcement.
class CapsuleRunner {
public:
    /// Run a capsule by name with optional arguments.
    static Result<pid_t, std::string> run(const std::string& name,
                                           const std::vector<std::string>& args = {});

    /// Stop a running capsule by name.
    static Result<void, std::string> stop(const std::string& name);

    /// List all running capsules with resource usage vs contract.
    static std::vector<RunningCapsule> list_running();

private:
    /// Pre-flight: verify all contracted resources are currently available.
    static Result<void, std::string> preflight_check(const CapsuleManifest& manifest);

    /// Pre-allocate VRAM via VPU ioctl if the contract specifies VRAM.
    static Result<void, std::string> preallocate_vram(const std::string& name, uint32_t vram_mb);

    /// Release pre-allocated VRAM.
    static void release_vram(const std::string& name);

    /// Create a cgroup for the capsule with limits from the resource contract.
    static Result<std::string, std::string> create_cgroup(const std::string& name,
                                                           const ResourceContract& contract);

    /// Remove the cgroup for a capsule.
    static void remove_cgroup(const std::string& name);

    /// Launch the binary inside the cgroup.
    static Result<pid_t, std::string> launch_in_cgroup(const std::string& cgroup_path,
                                                        const std::string& binary_path,
                                                        const std::vector<std::string>& args);

    /// Record a running capsule in the state file.
    static void record_running(const std::string& name, pid_t pid,
                               const ResourceContract& contract);

    /// Remove a capsule from the running state file.
    static void unrecord_running(const std::string& name);

    /// Read current resource usage for a pid.
    static RunningCapsule read_usage(const std::string& name, pid_t pid,
                                      const ResourceContract& contract,
                                      uint64_t start_time);

    /// Path to the running capsules state file.
    static constexpr const char* STATE_FILE = "/var/lib/straylight/capsule/running.json";
};

} // namespace straylight
