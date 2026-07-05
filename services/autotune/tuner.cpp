// services/autotune/tuner.cpp
#include "tuner.h"
#include <straylight/log.h>

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace straylight {

namespace fs = std::filesystem;

namespace {

bool token_present(const std::string& haystack, const std::string& needle) {
    std::istringstream iss(haystack);
    std::string token;
    while (iss >> token) {
        if (token == needle) return true;
    }
    return false;
}

std::string first_token(const std::string& text) {
    std::istringstream iss(text);
    std::string token;
    iss >> token;
    return token;
}

std::string select_supported_governor(const std::string& requested,
                                      const std::string& available) {
    if (available.empty() || token_present(available, requested)) return requested;

    for (const auto& fallback : {"schedutil", "powersave", "performance"}) {
        if (token_present(available, fallback)) return fallback;
    }

    return first_token(available);
}

bool write_sysfs_quiet(const std::string& path, const std::string& value) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return false;
    std::ofstream ofs(path);
    if (!ofs.is_open()) return false;
    ofs << value;
    ofs.flush();
    return !ofs.fail();
}

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

SystemTuner::SystemTuner() = default;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

bool SystemTuner::write_sysfs(const std::string& path, const std::string& value) {
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        SL_DEBUG("autotune: sysfs path not found (expected on macOS): {}", path);
        return false;
    }
    std::ofstream ofs(path);
    if (!ofs.is_open()) {
        SL_WARN("autotune: cannot open {} for writing (permission denied?)", path);
        return false;
    }
    ofs << value;
    ofs.flush();
    if (ofs.fail()) {
        SL_WARN("autotune: write to {} failed", path);
        return false;
    }
    SL_DEBUG("autotune: wrote '{}' -> {}", value, path);
    return true;
}

bool SystemTuner::write_sysctl(const std::string& key, const std::string& value) {
    std::string cmd = "sysctl -w " + key + "=" + value + " 2>/dev/null";
    auto [output, ok] = exec_cmd(cmd);
    if (!ok) {
        SL_DEBUG("autotune: sysctl {} = {} failed (expected on macOS)", key, value);
        return false;
    }
    SL_DEBUG("autotune: sysctl {} = {}", key, value);
    return true;
}

std::pair<std::string, bool> SystemTuner::exec_cmd(const std::string& cmd) {
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return {"", false};
    char buf[256];
    while (fgets(buf, sizeof(buf), pipe)) {
        result += buf;
    }
    int status = pclose(pipe);
    return {result, status == 0};
}

// ---------------------------------------------------------------------------
// Profile management
// ---------------------------------------------------------------------------

void SystemTuner::set_profile(const std::string& name) {
    std::lock_guard lock(mutex_);
    if (profile_mgr_.get(name)) {
        current_profile_name_ = name;
        SL_INFO("autotune: profile set to '{}'", name);
    } else {
        SL_WARN("autotune: unknown profile '{}', keeping '{}'", name, current_profile_name_);
    }
}

std::string SystemTuner::current_profile() const {
    std::lock_guard lock(mutex_);
    return current_profile_name_;
}

std::vector<std::string> SystemTuner::available_profiles() const {
    std::lock_guard lock(mutex_);
    return profile_mgr_.available();
}

// ---------------------------------------------------------------------------
// Full auto-tune pass
// ---------------------------------------------------------------------------

Result<void, std::string> SystemTuner::detect_and_tune() {
    SL_INFO("autotune: starting full auto-tune pass (profile='{}')", current_profile_name_);

    // Detect hardware
    hw_ = probe_.detect();

    // Apply each enabled subsystem
    if (features_.cpu) {
        auto r = tune_cpu();
        if (!r.has_value()) SL_WARN("autotune: cpu tuning failed: {}", r.error());
    }
    if (features_.memory) {
        auto r = tune_memory();
        if (!r.has_value()) SL_WARN("autotune: memory tuning failed: {}", r.error());
    }
    if (features_.io) {
        auto r = tune_io();
        if (!r.has_value()) SL_WARN("autotune: io tuning failed: {}", r.error());
    }
    if (features_.network) {
        auto r = tune_network();
        if (!r.has_value()) SL_WARN("autotune: network tuning failed: {}", r.error());
    }
    if (features_.gpu) {
        auto r = tune_gpu();
        if (!r.has_value()) SL_WARN("autotune: gpu tuning failed: {}", r.error());
    }
    if (features_.kernel) {
        auto r = tune_kernel();
        if (!r.has_value()) SL_WARN("autotune: kernel tuning failed: {}", r.error());
    }

    SL_INFO("autotune: auto-tune pass complete");
    return Result<void, std::string>::ok();
}

// ---------------------------------------------------------------------------
// CPU tuning
// ---------------------------------------------------------------------------

Result<void, std::string> SystemTuner::tune_cpu() {
    const auto* profile = profile_mgr_.get(current_profile_name_);
    if (!profile) return Result<void, std::string>::error("unknown profile: " + current_profile_name_);

    std::string available_governors;
    {
        std::ifstream govs("/sys/devices/system/cpu/cpu0/cpufreq/scaling_available_governors");
        std::getline(govs, available_governors);
    }

    const std::string selected_governor =
        select_supported_governor(profile->cpu_governor, available_governors);
    if (selected_governor != profile->cpu_governor) {
        SL_INFO("autotune: CPU governor '{}' unavailable (available: {}), using '{}'",
                profile->cpu_governor, available_governors, selected_governor);
    }

    SL_INFO("autotune: tuning CPU (governor={}, boost={})",
            selected_governor, profile->cpu_boost_enabled ? "on" : "off");

    // Determine number of CPUs
    int num_cpus = hw_.cpu.logical_cores;
    if (num_cpus <= 0) num_cpus = 1;

    // Set governor for each CPU
    int governor_write_failures = 0;
    for (int i = 0; i < num_cpus; i++) {
        std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpufreq/";
        if (!selected_governor.empty() &&
            !write_sysfs_quiet(base + "scaling_governor", selected_governor)) {
            governor_write_failures++;
        }

        // Set frequency bounds if the driver supports it
        if (profile->cpu_min_freq_pct > 0 && hw_.cpu.max_freq_mhz > 0) {
            int min_khz = (hw_.cpu.max_freq_mhz * 1000 * profile->cpu_min_freq_pct) / 100;
            write_sysfs(base + "scaling_min_freq", std::to_string(min_khz));
        }
        if (profile->cpu_max_freq_pct < 100 && hw_.cpu.max_freq_mhz > 0) {
            int max_khz = (hw_.cpu.max_freq_mhz * 1000 * profile->cpu_max_freq_pct) / 100;
            write_sysfs(base + "scaling_max_freq", std::to_string(max_khz));
        }
    }

    if (governor_write_failures > 0) {
        SL_WARN("autotune: CPU governor write failed on {}/{} CPUs",
                governor_write_failures, num_cpus);
    }

    // CPU boost control
    if (hw_.cpu.features.intel_pstate) {
        write_sysfs("/sys/devices/system/cpu/intel_pstate/no_turbo",
                     profile->cpu_boost_enabled ? "0" : "1");
    } else {
        write_sysfs("/sys/devices/system/cpu/cpufreq/boost",
                     profile->cpu_boost_enabled ? "1" : "0");
    }

    return Result<void, std::string>::ok();
}

// ---------------------------------------------------------------------------
// Memory tuning
// ---------------------------------------------------------------------------

Result<void, std::string> SystemTuner::tune_memory() {
    const auto* profile = profile_mgr_.get(current_profile_name_);
    if (!profile) return Result<void, std::string>::error("unknown profile: " + current_profile_name_);

    SL_INFO("autotune: tuning memory (swappiness={}, hugepages={}, THP={})",
            profile->swappiness, profile->nr_hugepages, profile->thp_enabled);

    // Swappiness
    write_sysfs("/proc/sys/vm/swappiness", std::to_string(profile->swappiness));

    // Dirty ratios
    write_sysfs("/proc/sys/vm/dirty_ratio", std::to_string(profile->dirty_ratio));
    write_sysfs("/proc/sys/vm/dirty_background_ratio", std::to_string(profile->dirty_background_ratio));

    // Transparent Huge Pages
    write_sysfs("/sys/kernel/mm/transparent_hugepage/enabled", profile->thp_enabled);

    // Static hugepages
    if (profile->nr_hugepages > 0) {
        write_sysfs("/proc/sys/vm/nr_hugepages", std::to_string(profile->nr_hugepages));
    }

    return Result<void, std::string>::ok();
}

// ---------------------------------------------------------------------------
// I/O tuning
// ---------------------------------------------------------------------------

Result<void, std::string> SystemTuner::tune_io() {
    const auto* profile = profile_mgr_.get(current_profile_name_);
    if (!profile) return Result<void, std::string>::error("unknown profile: " + current_profile_name_);

    SL_INFO("autotune: tuning I/O schedulers");

    for (const auto& dev : hw_.block_devices) {
        std::string sched_path = "/sys/block/" + dev.name + "/queue/scheduler";
        std::string ra_path    = "/sys/block/" + dev.name + "/queue/read_ahead_kb";

        std::string scheduler;
        int read_ahead_kb = 128;

        if (dev.transport == "nvme") {
            scheduler     = profile->io_scheduler_nvme;
            read_ahead_kb = profile->read_ahead_kb_nvme;
        } else if (!dev.rotational) {
            scheduler     = profile->io_scheduler_ssd;
            read_ahead_kb = profile->read_ahead_kb_ssd;
        } else {
            scheduler     = profile->io_scheduler_hdd;
            read_ahead_kb = profile->read_ahead_kb_hdd;
        }

        SL_DEBUG("autotune: {} -> scheduler={}, readahead={}KiB",
                 dev.name, scheduler, read_ahead_kb);

        write_sysfs(sched_path, scheduler);
        write_sysfs(ra_path, std::to_string(read_ahead_kb));
    }

    return Result<void, std::string>::ok();
}

// ---------------------------------------------------------------------------
// Network tuning
// ---------------------------------------------------------------------------

Result<void, std::string> SystemTuner::tune_network() {
    const auto* profile = profile_mgr_.get(current_profile_name_);
    if (!profile) return Result<void, std::string>::error("unknown profile: " + current_profile_name_);

    SL_INFO("autotune: tuning network (congestion={}, rmem_max={}, wmem_max={})",
            profile->tcp_congestion, profile->rmem_max, profile->wmem_max);

    // TCP congestion control
    write_sysctl("net.ipv4.tcp_congestion_control", profile->tcp_congestion);

    // Socket buffer sizes
    write_sysctl("net.core.rmem_max", std::to_string(profile->rmem_max));
    write_sysctl("net.core.wmem_max", std::to_string(profile->wmem_max));
    write_sysctl("net.core.rmem_default", std::to_string(profile->rmem_max / 2));
    write_sysctl("net.core.wmem_default", std::to_string(profile->wmem_max / 2));

    // TCP buffer auto-tuning range
    std::string tcp_rmem = "4096 " + std::to_string(profile->rmem_max / 4) + " " + std::to_string(profile->rmem_max);
    std::string tcp_wmem = "4096 " + std::to_string(profile->wmem_max / 4) + " " + std::to_string(profile->wmem_max);
    write_sysctl("net.ipv4.tcp_rmem", "\"" + tcp_rmem + "\"");
    write_sysctl("net.ipv4.tcp_wmem", "\"" + tcp_wmem + "\"");

    // Backlog
    write_sysctl("net.core.netdev_max_backlog", std::to_string(profile->netdev_max_backlog));

    // Enable BBR requires fq qdisc — set it if BBR selected
    if (profile->tcp_congestion == "bbr") {
        write_sysctl("net.core.default_qdisc", "fq");
    }

    // GRO/GSO per-interface via ethtool
    for (const auto& iface : hw_.net_interfaces) {
        if (profile->enable_gro && iface.supports_gro) {
            exec_cmd("ethtool -K " + iface.name + " gro on 2>/dev/null");
        }
        if (profile->enable_gso && iface.supports_gso) {
            exec_cmd("ethtool -K " + iface.name + " gso on 2>/dev/null");
        }

        // Ring buffer maximization
        if (iface.rx_ring_max > 0) {
            exec_cmd("ethtool -G " + iface.name + " rx " + std::to_string(iface.rx_ring_max) + " 2>/dev/null");
        }
        if (iface.tx_ring_max > 0) {
            exec_cmd("ethtool -G " + iface.name + " tx " + std::to_string(iface.tx_ring_max) + " 2>/dev/null");
        }
    }

    return Result<void, std::string>::ok();
}

// ---------------------------------------------------------------------------
// GPU tuning
// ---------------------------------------------------------------------------

Result<void, std::string> SystemTuner::tune_gpu() {
    const auto* profile = profile_mgr_.get(current_profile_name_);
    if (!profile) return Result<void, std::string>::error("unknown profile: " + current_profile_name_);

    if (hw_.gpus.empty()) {
        SL_INFO("autotune: no GPUs to tune");
        return Result<void, std::string>::ok();
    }

    SL_INFO("autotune: tuning {} GPU(s) (power={}, clocks={})",
            hw_.gpus.size(), profile->gpu_power_mode, profile->gpu_clock_mode);

    for (size_t i = 0; i < hw_.gpus.size(); i++) {
        const auto& gpu = hw_.gpus[i];

        if (gpu.vendor == "nvidia" && gpu.has_nvidia_smi) {
            std::string idx = std::to_string(i);

            // Persistence mode (keeps driver loaded)
            exec_cmd("nvidia-smi -i " + idx + " -pm 1 2>/dev/null");

            // Power limit
            if (profile->gpu_power_mode == "max") {
                // Query max power limit and set to it
                auto [max_pl, ok] = exec_cmd(
                    "nvidia-smi -i " + idx + " --query-gpu=power.max_limit --format=csv,noheader,nounits 2>/dev/null");
                if (ok && !max_pl.empty()) {
                    // Trim
                    while (!max_pl.empty() && (max_pl.back() == '\n' || max_pl.back() == ' '))
                        max_pl.pop_back();
                    exec_cmd("nvidia-smi -i " + idx + " -pl " + max_pl + " 2>/dev/null");
                    SL_INFO("autotune: GPU {} power limit set to {} W (max)", i, max_pl);
                }
            } else if (profile->gpu_power_mode == "min") {
                auto [min_pl, ok] = exec_cmd(
                    "nvidia-smi -i " + idx + " --query-gpu=power.min_limit --format=csv,noheader,nounits 2>/dev/null");
                if (ok && !min_pl.empty()) {
                    while (!min_pl.empty() && (min_pl.back() == '\n' || min_pl.back() == ' '))
                        min_pl.pop_back();
                    exec_cmd("nvidia-smi -i " + idx + " -pl " + min_pl + " 2>/dev/null");
                    SL_INFO("autotune: GPU {} power limit set to {} W (min)", i, min_pl);
                }
            }
            // "auto" leaves power limit at driver default

            // Application clocks
            if (profile->gpu_clock_mode == "max") {
                // Query max clocks and set
                auto [clocks, ok] = exec_cmd(
                    "nvidia-smi -i " + idx + " --query-gpu=clocks.max.graphics,clocks.max.memory "
                    "--format=csv,noheader,nounits 2>/dev/null");
                if (ok && !clocks.empty()) {
                    // Parse "graphics_max, memory_max"
                    std::istringstream iss(clocks);
                    std::string gfx_max, mem_max;
                    if (std::getline(iss, gfx_max, ',') && std::getline(iss, mem_max)) {
                        auto trim = [](std::string& s) {
                            while (!s.empty() && (s.front() == ' ')) s.erase(s.begin());
                            while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
                        };
                        trim(gfx_max); trim(mem_max);
                        exec_cmd("nvidia-smi -i " + idx + " -ac " + mem_max + "," + gfx_max + " 2>/dev/null");
                        SL_INFO("autotune: GPU {} clocks set to mem={} gfx={} (max)", i, mem_max, gfx_max);
                    }
                }
            } else if (profile->gpu_clock_mode == "base") {
                // Reset application clocks to default
                exec_cmd("nvidia-smi -i " + idx + " -rac 2>/dev/null");
                SL_INFO("autotune: GPU {} clocks reset to default", i);
            }

        } else if (gpu.vendor == "amd" && gpu.has_rocm_smi) {
            std::string idx = std::to_string(i);

            if (profile->gpu_power_mode == "max") {
                // Set performance level to high
                exec_cmd("rocm-smi -d " + idx + " --setperflevel high 2>/dev/null");
                SL_INFO("autotune: AMD GPU {} set to high performance", i);
            } else if (profile->gpu_power_mode == "min") {
                exec_cmd("rocm-smi -d " + idx + " --setperflevel low 2>/dev/null");
                SL_INFO("autotune: AMD GPU {} set to low power", i);
            } else {
                exec_cmd("rocm-smi -d " + idx + " --setperflevel auto 2>/dev/null");
            }

            // AMD sysfs power cap
            std::string power_cap_path = "/sys/class/drm/card" + idx + "/device/hwmon/hwmon0/power1_cap";
            if (profile->gpu_power_limit_watts > 0) {
                // Power cap is in microwatts
                uint64_t cap_uw = static_cast<uint64_t>(profile->gpu_power_limit_watts) * 1000000ULL;
                write_sysfs(power_cap_path, std::to_string(cap_uw));
            }

        } else {
            SL_DEBUG("autotune: GPU {} ({}) has no management tool, skipping", i, gpu.name);
        }
    }

    return Result<void, std::string>::ok();
}

// ---------------------------------------------------------------------------
// Kernel sysctl tuning
// ---------------------------------------------------------------------------

Result<void, std::string> SystemTuner::tune_kernel() {
    const auto* profile = profile_mgr_.get(current_profile_name_);
    if (!profile) return Result<void, std::string>::error("unknown profile: " + current_profile_name_);

    SL_INFO("autotune: tuning kernel sysctls");

    // File descriptor limits
    write_sysctl("fs.file-max", std::to_string(profile->fs_file_max));

    // Scheduler granularity (CFS)
    write_sysctl("kernel.sched_min_granularity_ns",
                 std::to_string(profile->kernel_sched_min_granularity_ns));
    write_sysctl("kernel.sched_wakeup_granularity_ns",
                 std::to_string(profile->kernel_sched_wakeup_granularity_ns));

    // Network core
    write_sysctl("net.core.somaxconn", std::to_string(profile->somaxconn));

    // VM
    write_sysctl("vm.max_map_count", std::to_string(profile->vm_max_map_count));

    // Additional hardening/performance sysctls
    write_sysctl("net.ipv4.tcp_fastopen", "3");             // Enable TFO for client + server
    write_sysctl("net.ipv4.tcp_slow_start_after_idle", "0"); // Don't reset cwnd after idle
    write_sysctl("net.ipv4.tcp_mtu_probing", "1");           // Enable PLPMTUD
    write_sysctl("net.ipv4.tcp_timestamps", "1");
    write_sysctl("net.ipv4.tcp_sack", "1");
    write_sysctl("net.ipv4.tcp_window_scaling", "1");

    // Disable SYN cookies only on performance/ml-training (they add overhead)
    if (current_profile_name_ == "performance" || current_profile_name_ == "ml-training") {
        write_sysctl("net.ipv4.tcp_syncookies", "0");
    } else {
        write_sysctl("net.ipv4.tcp_syncookies", "1");
    }

    // Kernel hardening — keep even in performance mode
    write_sysctl("kernel.randomize_va_space", "2");
    write_sysctl("kernel.kptr_restrict", "1");

    return Result<void, std::string>::ok();
}

} // namespace straylight
