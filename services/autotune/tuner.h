// services/autotune/tuner.h
#pragma once

#include <straylight/result.h>
#include "hardware_probe.h"
#include "profiles.h"

#include <mutex>
#include <string>
#include <vector>

namespace straylight {

/// Core tuning engine. Applies system-level tuning based on a profile and
/// detected hardware. All write operations target /proc and /sys paths;
/// on macOS (or when sysfs is absent) they log the intended writes and skip.
class SystemTuner {
public:
    SystemTuner();

    /// Run a full auto-tune pass: detect hardware, select or apply current
    /// profile, tune every enabled subsystem.
    Result<void, std::string> detect_and_tune();

    /// Individual subsystem tuners. Each reads the current profile and
    /// hardware inventory, then writes the appropriate sysfs/proc knobs.
    Result<void, std::string> tune_cpu();
    Result<void, std::string> tune_memory();
    Result<void, std::string> tune_io();
    Result<void, std::string> tune_network();
    Result<void, std::string> tune_gpu();
    Result<void, std::string> tune_kernel();

    /// Switch to a named profile. Does NOT apply until tune is called.
    void set_profile(const std::string& name);

    /// Return the name of the currently active profile.
    std::string current_profile() const;

    /// List all available profile names.
    std::vector<std::string> available_profiles() const;

    /// Access the hardware inventory (populated after detect_and_tune()).
    const HardwareInventory& hardware() const { return hw_; }

    /// Feature enable/disable flags (set from config).
    struct Features {
        bool cpu     = true;
        bool memory  = true;
        bool io      = true;
        bool network = true;
        bool gpu     = true;
        bool kernel  = true;
    };
    Features& features() { return features_; }

private:
    /// Write a value to a sysfs/proc path. Returns false if the write fails
    /// (file missing, permission denied, etc.) — logged but non-fatal.
    bool write_sysfs(const std::string& path, const std::string& value);

    /// Run a sysctl write. Returns false on failure.
    bool write_sysctl(const std::string& key, const std::string& value);

    /// Execute a shell command. Returns the stdout output and whether
    /// the exit code was 0.
    std::pair<std::string, bool> exec_cmd(const std::string& cmd);

    HardwareProbe probe_;
    HardwareInventory hw_;
    ProfileManager profile_mgr_;
    std::string current_profile_name_ = "balanced";
    Features features_;
    mutable std::mutex mutex_;
};

} // namespace straylight
