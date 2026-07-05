// services/autotune/profiles.cpp
#include "profiles.h"

#include <algorithm>

namespace straylight {

ProfileManager::ProfileManager() {
    init_builtin_profiles();
}

const TuningProfile* ProfileManager::get(const std::string& name) const {
    auto it = profiles_.find(name);
    return (it != profiles_.end()) ? &it->second : nullptr;
}

std::vector<std::string> ProfileManager::available() const {
    std::vector<std::string> names;
    names.reserve(profiles_.size());
    for (const auto& [name, _] : profiles_) {
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

void ProfileManager::add(TuningProfile profile) {
    std::string key = profile.name;
    profiles_[std::move(key)] = std::move(profile);
}

void ProfileManager::init_builtin_profiles() {
    // -----------------------------------------------------------------------
    // Performance: maximum throughput, no power saving
    // -----------------------------------------------------------------------
    {
        TuningProfile p;
        p.name = "performance";
        p.cpu_governor       = "performance";
        p.cpu_boost_enabled  = true;
        p.cpu_min_freq_pct   = 100;
        p.cpu_max_freq_pct   = 100;

        p.swappiness             = 10;
        p.dirty_ratio            = 40;
        p.dirty_background_ratio = 20;
        p.thp_enabled            = "always";
        p.nr_hugepages           = 1024;

        p.io_scheduler_nvme  = "none";
        p.io_scheduler_ssd   = "mq-deadline";
        p.io_scheduler_hdd   = "mq-deadline";
        p.read_ahead_kb_nvme = 256;
        p.read_ahead_kb_ssd  = 256;
        p.read_ahead_kb_hdd  = 512;

        p.tcp_congestion     = "bbr";
        p.rmem_max           = 33554432;  // 32 MiB
        p.wmem_max           = 33554432;
        p.netdev_max_backlog = 10000;
        p.enable_gro         = true;
        p.enable_gso         = true;

        p.gpu_power_mode     = "max";
        p.gpu_clock_mode     = "max";

        p.fs_file_max                       = 2097152;
        p.kernel_sched_min_granularity_ns   = 1000000;
        p.kernel_sched_wakeup_granularity_ns= 1500000;
        p.somaxconn                         = 8192;
        p.vm_max_map_count                  = 524288;

        profiles_[p.name] = std::move(p);
    }

    // -----------------------------------------------------------------------
    // Balanced: sensible defaults, moderate power saving
    // -----------------------------------------------------------------------
    {
        TuningProfile p;
        p.name = "balanced";
        p.cpu_governor       = "ondemand";
        p.cpu_boost_enabled  = true;
        p.cpu_min_freq_pct   = 0;
        p.cpu_max_freq_pct   = 100;

        p.swappiness             = 60;
        p.dirty_ratio            = 20;
        p.dirty_background_ratio = 10;
        p.thp_enabled            = "madvise";
        p.nr_hugepages           = 256;

        p.io_scheduler_nvme  = "none";
        p.io_scheduler_ssd   = "mq-deadline";
        p.io_scheduler_hdd   = "bfq";
        p.read_ahead_kb_nvme = 128;
        p.read_ahead_kb_ssd  = 128;
        p.read_ahead_kb_hdd  = 256;

        p.tcp_congestion     = "bbr";
        p.rmem_max           = 16777216;
        p.wmem_max           = 16777216;
        p.netdev_max_backlog = 5000;
        p.enable_gro         = true;
        p.enable_gso         = true;

        p.gpu_power_mode     = "auto";
        p.gpu_clock_mode     = "auto";

        p.fs_file_max                       = 1048576;
        p.kernel_sched_min_granularity_ns   = 3000000;
        p.kernel_sched_wakeup_granularity_ns= 4000000;
        p.somaxconn                         = 4096;
        p.vm_max_map_count                  = 262144;

        profiles_[p.name] = std::move(p);
    }

    // -----------------------------------------------------------------------
    // Powersave: minimize power consumption
    // -----------------------------------------------------------------------
    {
        TuningProfile p;
        p.name = "powersave";
        p.cpu_governor       = "powersave";
        p.cpu_boost_enabled  = false;
        p.cpu_min_freq_pct   = 0;
        p.cpu_max_freq_pct   = 70;

        p.swappiness             = 80;
        p.dirty_ratio            = 10;
        p.dirty_background_ratio = 5;
        p.thp_enabled            = "never";
        p.nr_hugepages           = 0;

        p.io_scheduler_nvme  = "none";
        p.io_scheduler_ssd   = "bfq";
        p.io_scheduler_hdd   = "bfq";
        p.read_ahead_kb_nvme = 64;
        p.read_ahead_kb_ssd  = 64;
        p.read_ahead_kb_hdd  = 128;

        p.tcp_congestion     = "cubic";
        p.rmem_max           = 4194304;
        p.wmem_max           = 4194304;
        p.netdev_max_backlog = 1000;
        p.enable_gro         = true;
        p.enable_gso         = false;

        p.gpu_power_mode     = "min";
        p.gpu_power_limit_watts = 0;   // let driver choose minimum
        p.gpu_clock_mode     = "base";

        p.fs_file_max                       = 524288;
        p.kernel_sched_min_granularity_ns   = 4000000;
        p.kernel_sched_wakeup_granularity_ns= 5000000;
        p.somaxconn                         = 2048;
        p.vm_max_map_count                  = 65536;

        profiles_[p.name] = std::move(p);
    }

    // -----------------------------------------------------------------------
    // ML Training: max GPU throughput, large memory buffers, high network
    // -----------------------------------------------------------------------
    {
        TuningProfile p;
        p.name = "ml-training";
        p.cpu_governor       = "performance";
        p.cpu_boost_enabled  = true;
        p.cpu_min_freq_pct   = 100;
        p.cpu_max_freq_pct   = 100;

        p.swappiness             = 1;
        p.dirty_ratio            = 40;
        p.dirty_background_ratio = 20;
        p.thp_enabled            = "always";
        p.nr_hugepages           = 4096;

        p.io_scheduler_nvme  = "none";
        p.io_scheduler_ssd   = "mq-deadline";
        p.io_scheduler_hdd   = "mq-deadline";
        p.read_ahead_kb_nvme = 512;
        p.read_ahead_kb_ssd  = 512;
        p.read_ahead_kb_hdd  = 1024;

        p.tcp_congestion     = "bbr";
        p.rmem_max           = 67108864;  // 64 MiB (for NCCL/RDMA-like traffic)
        p.wmem_max           = 67108864;
        p.netdev_max_backlog = 30000;
        p.enable_gro         = true;
        p.enable_gso         = true;

        p.gpu_power_mode        = "max";
        p.gpu_power_limit_watts = 0;      // max (no cap)
        p.gpu_clock_mode        = "max";

        p.fs_file_max                       = 4194304;
        p.kernel_sched_min_granularity_ns   = 1000000;
        p.kernel_sched_wakeup_granularity_ns= 1500000;
        p.somaxconn                         = 16384;
        p.vm_max_map_count                  = 1048576;

        profiles_[p.name] = std::move(p);
    }

    // -----------------------------------------------------------------------
    // Desktop: low latency, responsive I/O, balanced power
    // -----------------------------------------------------------------------
    {
        TuningProfile p;
        p.name = "desktop";
        p.cpu_governor       = "schedutil";
        p.cpu_boost_enabled  = true;
        p.cpu_min_freq_pct   = 0;
        p.cpu_max_freq_pct   = 100;

        p.swappiness             = 30;
        p.dirty_ratio            = 15;
        p.dirty_background_ratio = 5;
        p.thp_enabled            = "madvise";
        p.nr_hugepages           = 128;

        p.io_scheduler_nvme  = "none";
        p.io_scheduler_ssd   = "bfq";
        p.io_scheduler_hdd   = "bfq";
        p.read_ahead_kb_nvme = 128;
        p.read_ahead_kb_ssd  = 128;
        p.read_ahead_kb_hdd  = 256;

        p.tcp_congestion     = "bbr";
        p.rmem_max           = 16777216;
        p.wmem_max           = 16777216;
        p.netdev_max_backlog = 5000;
        p.enable_gro         = true;
        p.enable_gso         = true;

        p.gpu_power_mode     = "auto";
        p.gpu_clock_mode     = "auto";

        // Low-latency scheduler settings for desktop responsiveness
        p.fs_file_max                       = 1048576;
        p.kernel_sched_min_granularity_ns   = 750000;   // 0.75ms — snappy interactivity
        p.kernel_sched_wakeup_granularity_ns= 1000000;  // 1ms
        p.somaxconn                         = 4096;
        p.vm_max_map_count                  = 262144;

        profiles_[p.name] = std::move(p);
    }
}

} // namespace straylight
