// services/autotune/profiles.h
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace straylight {

/// Complete set of tuning knobs that define a system profile.
struct TuningProfile {
    std::string name;

    // CPU
    std::string cpu_governor;         // "performance", "powersave", "ondemand", "schedutil"
    bool cpu_boost_enabled = true;
    int cpu_min_freq_pct   = 0;       // 0 = no override
    int cpu_max_freq_pct   = 100;

    // Memory
    int swappiness              = 60;
    int dirty_ratio             = 20;
    int dirty_background_ratio  = 10;
    std::string thp_enabled     = "madvise";  // "always", "madvise", "never"
    int nr_hugepages            = 0;

    // I/O
    std::string io_scheduler_nvme = "none";   // "none" (mq-deadline), "bfq", "kyber"
    std::string io_scheduler_ssd  = "mq-deadline";
    std::string io_scheduler_hdd  = "bfq";
    int read_ahead_kb_nvme        = 128;
    int read_ahead_kb_ssd         = 128;
    int read_ahead_kb_hdd         = 256;

    // Network
    std::string tcp_congestion  = "bbr";
    int rmem_max                = 16777216;   // 16 MiB
    int wmem_max                = 16777216;
    int netdev_max_backlog      = 5000;
    bool enable_gro             = true;
    bool enable_gso             = true;

    // GPU
    std::string gpu_power_mode  = "auto";     // "auto", "max", "min"
    int gpu_power_limit_watts   = 0;          // 0 = no override
    std::string gpu_clock_mode  = "auto";     // "auto", "max", "base"

    // Kernel sysctl
    int fs_file_max             = 1048576;
    int kernel_sched_min_granularity_ns = 3000000;
    int kernel_sched_wakeup_granularity_ns = 4000000;
    int somaxconn               = 4096;
    int vm_max_map_count        = 262144;
};

/// Manages the set of built-in and custom profiles.
class ProfileManager {
public:
    ProfileManager();

    /// Get a profile by name. Returns nullptr if not found.
    const TuningProfile* get(const std::string& name) const;

    /// List all available profile names.
    std::vector<std::string> available() const;

    /// Register a custom profile.
    void add(TuningProfile profile);

private:
    std::unordered_map<std::string, TuningProfile> profiles_;

    void init_builtin_profiles();
};

} // namespace straylight
