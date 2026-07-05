/**
 * StrayLight Hotpatch — Patch Engine (header)
 *
 * Core patching logic for three patch targets:
 *   1. Kernel modules via ftrace/livepatch sysfs interface
 *   2. DaemonBase daemons via SIGHUP + IPC reload
 *   3. Config files via unified diff application
 */
#pragma once

#include "patch_registry.h"
#include "straylight/result.h"

#include <string>
#include <vector>

namespace straylight::hotpatch {

class PatchEngine {
public:
    explicit PatchEngine(PatchRegistry& registry);

    /**
     * Apply a kernel livepatch to a loaded module.
     * Uses /sys/kernel/livepatch/ to manage the patch lifecycle.
     */
    Result<std::string, std::string> apply_kernel_patch(
        const std::string& module_name,
        const std::string& patch_path);

    /**
     * Send a reload command to a running DaemonBase service.
     * Sends SIGHUP to trigger on_reload(), then verifies via IPC.
     */
    Result<std::string, std::string> apply_daemon_patch(
        const std::string& service_name,
        const std::string& patch_data);

    /**
     * Apply a unified diff to a config file and notify the owning service.
     */
    Result<std::string, std::string> apply_config_patch(
        const std::string& config_path,
        const std::string& diff);

    /**
     * Roll back a previously applied patch by ID.
     */
    VoidResult<> rollback(const std::string& patch_id);

    /**
     * List all patches matching an optional status filter.
     */
    std::vector<PatchRecord> list(
        const std::string& status_filter = "") const;

    /**
     * Get full status of a single patch.
     */
    Result<PatchRecord, std::string> status(
        const std::string& patch_id) const;

private:
    PatchRegistry& registry_;

    /** Read the PID from a daemon's pidfile. */
    Result<pid_t, std::string> read_daemon_pid(const std::string& service_name);

    /** Apply a unified diff to a file, return the original content for rollback. */
    Result<std::string, std::string> apply_unified_diff(
        const std::string& file_path, const std::string& diff);

    /** Restore a file from rollback data. */
    VoidResult<> restore_file(const std::string& path,
                              const std::string& original_content);

    /** Remove a kernel livepatch module. */
    VoidResult<> remove_kernel_patch(const std::string& module_name);
};

} // namespace straylight::hotpatch
