/**
 * StrayLight Quota — Unified Resource Budget Engine (implementation)
 */

#include "quota_engine.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace straylight::quota {
namespace {

bool read_uint_file(const std::string& path, uint64_t& value) {
    std::ifstream in(path);
    if (!in) return false;
    in >> value;
    return !in.fail();
}

}

QuotaEngine::QuotaEngine(QuotaConfig& config, CgroupController& cgroup)
    : config_(config), cgroup_(cgroup) {}

VoidResult<> QuotaEngine::set_quota(
        const std::string& app_name, pid_t pid,
        const ResourceQuota& quota) {
    std::lock_guard<std::mutex> lock(mu_);

    // Create cgroup for the app
    auto r = cgroup_.create_cgroup(app_name);
    if (!r) {
        fprintf(stderr, "[quota] warning: cgroup create for %s: %s\n",
                app_name.c_str(), r.error().c_str());
        // Non-fatal — might already exist or we're not root
    }

    // Assign PID to cgroup
    if (pid > 0) {
        cgroup_.assign_pid(app_name, pid);
    }

    // Apply cgroup limits
    if (quota.cpu_percent < 100.0) {
        cgroup_.set_cpu_limit(app_name, quota.cpu_percent);
    }
    if (quota.ram_bytes > 0) {
        cgroup_.set_memory_limit(app_name, quota.ram_bytes);
    }
    if (quota.disk_iops > 0 || quota.net_bandwidth > 0) {
        cgroup_.set_io_limit(app_name, quota.disk_iops, quota.net_bandwidth);
    }

    // Update config
    config_.set_quota(app_name, quota);

    // Track the app
    TrackedApp& tracked = tracked_[app_name];
    tracked.app_name = app_name;
    tracked.pid = pid;
    tracked.quota = quota;
    tracked.prev_sample = std::chrono::steady_clock::now();

    return VoidResult<>::ok();
}

void QuotaEngine::enforce() {
    std::lock_guard<std::mutex> lock(mu_);

    auto policy = config_.get_policy();
    if (!enforcement_enabled_ || !policy.enforcement_enabled) return;

    for (auto& [name, app] : tracked_) {
        // Verify process is still alive
        if (app.pid > 0 && kill(app.pid, 0) != 0) {
            // Process died — skip
            continue;
        }

        // Sample current usage
        AppUsage usage = sample_usage(app);
        app.last_usage = usage;

        // Determine and execute enforcement action
        EnforcementAction action = determine_action(usage, app.quota, policy);
        if (action != EnforcementAction::None) {
            execute_action(app, action);
            usage.last_action = action;
            app.last_usage = usage;
        }
    }
}

Result<AppUsage, std::string> QuotaEngine::get_usage(
        const std::string& app_name) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = tracked_.find(app_name);
    if (it == tracked_.end()) {
        return Result<AppUsage, std::string>::error(
            "app not tracked: " + app_name);
    }

    // Freshen the usage data
    AppUsage usage = sample_usage(it->second);
    it->second.last_usage = usage;
    return Result<AppUsage, std::string>::ok(usage);
}

std::vector<AppUsage> QuotaEngine::list_all_usage() {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<AppUsage> result;
    result.reserve(tracked_.size());
    for (auto& [_, app] : tracked_) {
        result.push_back(app.last_usage);
    }
    return result;
}

std::vector<Violation> QuotaEngine::get_violations(size_t max_count) const {
    std::lock_guard<std::mutex> lock(mu_);
    size_t start = 0;
    if (violations_.size() > max_count)
        start = violations_.size() - max_count;
    return std::vector<Violation>(
        violations_.begin() + static_cast<ptrdiff_t>(start),
        violations_.end());
}

void QuotaEngine::set_enforcement_enabled(bool enabled) {
    enforcement_enabled_ = enabled;
}

bool QuotaEngine::is_enforcement_enabled() const {
    return enforcement_enabled_;
}

// ── Private implementation ──────────────────────────────────────────

AppUsage QuotaEngine::sample_usage(TrackedApp& app) {
    AppUsage usage{};
    usage.app_name = app.app_name;
    usage.pid = app.pid;

    // Read cgroup usage
    auto cg_result = cgroup_.get_usage(app.app_name);
    if (cg_result) {
        const auto& cg = cg_result.value();

        // Calculate CPU % from delta of cpu.stat usage_usec
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            now - app.prev_sample);
        uint64_t cpu_usec = static_cast<uint64_t>(cg.cpu_percent * 1000000.0);
        if (elapsed.count() > 0 && app.prev_cpu_usec > 0) {
            uint64_t delta_usec = cpu_usec - app.prev_cpu_usec;
            usage.cpu_percent = (static_cast<double>(delta_usec) /
                                 static_cast<double>(elapsed.count())) * 100.0;
        }
        app.prev_cpu_usec = cpu_usec;
        app.prev_sample = now;

        usage.ram_bytes = cg.ram_bytes;
        usage.disk_iops = cg.disk_iops;
        usage.net_bandwidth = cg.net_rx_bytes + cg.net_tx_bytes;
    }

    // Read VPU VRAM usage
    if (app.pid > 0) {
        usage.vram_bytes = read_vram_usage(app.pid);
        usage.gpu_compute_percent = read_gpu_usage(app.pid);
        usage.compositor_fps = read_compositor_fps(app.pid);
    }

    return usage;
}

uint64_t QuotaEngine::read_vram_usage(pid_t pid) {
    uint64_t bytes = 0;

    // Preferred future ABI: per-process usage. Keep it if a newer VPU module
    // grows the interface, but do not require it for the current kernel.
    const std::string pid_path =
        "/sys/devices/virtual/straylight-vpu/vram/proc/" +
        std::to_string(pid) + "/usage_bytes";
    if (read_uint_file(pid_path, bytes)) return bytes;

    // Live Z6 ABI: aggregate VPU state under /sys/kernel/straylight-vpu/gpu0.
    if (read_uint_file("/sys/kernel/straylight-vpu/gpu0/vram_used", bytes)) {
        return bytes;
    }

    // Last resort: derive allocated slab bytes from slab_usage.
    std::ifstream slabs("/sys/kernel/straylight-vpu/slab_usage");
    std::string line;
    uint64_t total = 0;
    while (std::getline(slabs, line)) {
        uint64_t block_size = 0;
        uint64_t used = 0;
        if (std::sscanf(line.c_str(),
                        "order%*[^ ] block_size=%lu used=%lu/%*u",
                        &block_size,
                        &used) == 2) {
            total += block_size * used;
        }
    }
    return total;
}

double QuotaEngine::read_gpu_usage(pid_t pid) {
    // Read from DRM render node stats or StrayLight GPU driver
    // /sys/devices/virtual/straylight-gpu/compute/proc/<pid>/usage_percent
    std::string path = "/sys/devices/virtual/straylight-gpu/compute/proc/" +
                       std::to_string(pid) + "/usage_percent";
    std::ifstream in(path);
    if (!in) return 0.0;
    double pct = 0.0;
    in >> pct;
    return pct;
}

double QuotaEngine::read_compositor_fps(pid_t pid) {
    // StrayLight compositor reports per-client frame rates via
    // /var/run/straylight/compositor/client/<pid>/fps
    std::string path = "/var/run/straylight/compositor/client/" +
                       std::to_string(pid) + "/fps";
    std::ifstream in(path);
    if (!in) return 0.0;
    double fps = 0.0;
    in >> fps;
    return fps;
}

EnforcementAction QuotaEngine::determine_action(
        const AppUsage& usage,
        const ResourceQuota& quota,
        const EnforcementPolicy& policy) {
    // Check each resource dimension
    auto check = [&](double used, double limit, const std::string& resource)
            -> EnforcementAction {
        if (limit <= 0.0) return EnforcementAction::None;
        double pct = (used / limit) * 100.0;
        if (pct >= policy.kill_threshold) {
            record_violation(usage.app_name, resource, pct,
                             EnforcementAction::Kill);
            return EnforcementAction::Kill;
        }
        if (pct >= policy.throttle_threshold) {
            record_violation(usage.app_name, resource, pct,
                             EnforcementAction::Throttle);
            return EnforcementAction::Throttle;
        }
        if (pct >= policy.warn_threshold) {
            record_violation(usage.app_name, resource, pct,
                             EnforcementAction::Warn);
            return EnforcementAction::Warn;
        }
        return EnforcementAction::None;
    };

    // Track the most severe action across all resources
    EnforcementAction worst = EnforcementAction::None;

    auto escalate = [&](EnforcementAction a) {
        if (static_cast<int>(a) > static_cast<int>(worst)) worst = a;
    };

    escalate(check(usage.cpu_percent, quota.cpu_percent, "cpu"));
    if (quota.ram_bytes > 0)
        escalate(check(static_cast<double>(usage.ram_bytes),
                        static_cast<double>(quota.ram_bytes), "ram"));
    if (quota.vram_bytes > 0)
        escalate(check(static_cast<double>(usage.vram_bytes),
                        static_cast<double>(quota.vram_bytes), "vram"));
    escalate(check(usage.gpu_compute_percent,
                    quota.gpu_compute_percent, "gpu"));
    if (quota.disk_iops > 0)
        escalate(check(static_cast<double>(usage.disk_iops),
                        static_cast<double>(quota.disk_iops), "disk_iops"));
    if (quota.net_bandwidth > 0)
        escalate(check(static_cast<double>(usage.net_bandwidth),
                        static_cast<double>(quota.net_bandwidth), "net"));
    if (quota.compositor_fps > 0.0)
        escalate(check(usage.compositor_fps,
                        quota.compositor_fps, "compositor_fps"));

    return worst;
}

void QuotaEngine::execute_action(TrackedApp& app, EnforcementAction action) {
    switch (action) {
        case EnforcementAction::None:
            break;

        case EnforcementAction::Warn:
            app.warn_count++;
            fprintf(stderr, "[quota] WARNING: %s approaching quota limit "
                    "(warned %d times)\n",
                    app.app_name.c_str(), app.warn_count);
            break;

        case EnforcementAction::Throttle: {
            app.throttle_count++;
            fprintf(stderr, "[quota] THROTTLE: %s — reducing CPU limit\n",
                    app.app_name.c_str());
            // Reduce CPU to 50% of configured quota
            double throttled_cpu = app.quota.cpu_percent * 0.5;
            if (throttled_cpu < 5.0) throttled_cpu = 5.0;
            cgroup_.set_cpu_limit(app.app_name, throttled_cpu);
            break;
        }

        case EnforcementAction::Suspend:
            if (app.pid > 0) {
                fprintf(stderr, "[quota] SUSPEND: %s (pid %d)\n",
                        app.app_name.c_str(), app.pid);
                kill(app.pid, SIGSTOP);
            }
            break;

        case EnforcementAction::Kill:
            if (app.pid > 0) {
                fprintf(stderr, "[quota] KILL: %s (pid %d) — quota exceeded\n",
                        app.app_name.c_str(), app.pid);
                kill(app.pid, SIGTERM);
                // Give 2 seconds then SIGKILL
                usleep(2000000);
                if (kill(app.pid, 0) == 0) {
                    kill(app.pid, SIGKILL);
                }
            }
            break;
    }
}

void QuotaEngine::record_violation(
        const std::string& app, const std::string& resource,
        double usage_pct, EnforcementAction action) {
    Violation v{};
    v.app_name = app;
    v.resource = resource;
    v.usage_percent = usage_pct;
    v.action = action;
    v.timestamp = now_iso8601();
    violations_.push_back(std::move(v));

    // Cap at 10000 violations
    if (violations_.size() > 10000) {
        violations_.erase(violations_.begin(),
                          violations_.begin() + static_cast<ptrdiff_t>(
                              violations_.size() - 5000));
    }
}

std::string QuotaEngine::now_iso8601() {
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&tt, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

} // namespace straylight::quota
