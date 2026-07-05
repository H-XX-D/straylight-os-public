/**
 * StrayLight Quota — Unified Resource Budget Engine (header)
 *
 * Enforces per-app quotas across CPU, RAM, VRAM, GPU, disk I/O,
 * network bandwidth, and compositor frame budget.
 *
 * Reads actual usage from cgroup v2 controllers, VPU sysfs,
 * and compositor D-Bus, then takes enforcement actions.
 */
#pragma once

#include "cgroup_controller.h"
#include "quota_config.h"
#include "straylight/result.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace straylight::quota {

/** Enforcement action taken on a quota violation. */
enum class EnforcementAction { None, Warn, Throttle, Suspend, Kill };

inline std::string action_str(EnforcementAction a) {
    switch (a) {
        case EnforcementAction::None:     return "none";
        case EnforcementAction::Warn:     return "warn";
        case EnforcementAction::Throttle: return "throttle";
        case EnforcementAction::Suspend:  return "suspend";
        case EnforcementAction::Kill:     return "kill";
    }
    return "unknown";
}

/** Current usage snapshot for an application. */
struct AppUsage {
    std::string app_name;
    pid_t       pid{0};
    double      cpu_percent{0.0};
    uint64_t    ram_bytes{0};
    uint64_t    vram_bytes{0};
    double      gpu_compute_percent{0.0};
    uint64_t    disk_iops{0};
    uint64_t    net_bandwidth{0};
    double      compositor_fps{0.0};
    EnforcementAction last_action{EnforcementAction::None};
};

/** Violation record. */
struct Violation {
    std::string app_name;
    std::string resource;         // "cpu", "ram", "vram", etc.
    double      usage_percent;     // usage as % of quota
    EnforcementAction action;
    std::string timestamp;
};

class QuotaEngine {
public:
    QuotaEngine(QuotaConfig& config, CgroupController& cgroup);

    /**
     * Set quota for an app (by name or PID).
     * Creates cgroup if it doesn't exist and applies limits.
     */
    VoidResult<> set_quota(const std::string& app_name, pid_t pid,
                            const ResourceQuota& quota);

    /**
     * Main enforcement tick. Called every daemon tick.
     * Reads actual usage, compares to quotas, takes action.
     */
    void enforce();

    /**
     * Get current resource usage for an app.
     */
    Result<AppUsage, std::string> get_usage(const std::string& app_name);

    /**
     * List all tracked apps and their usage.
     */
    std::vector<AppUsage> list_all_usage();

    /**
     * Get recent violations.
     */
    std::vector<Violation> get_violations(size_t max_count = 50) const;

    /**
     * Enable or disable enforcement globally.
     */
    void set_enforcement_enabled(bool enabled);
    bool is_enforcement_enabled() const;

private:
    QuotaConfig& config_;
    CgroupController& cgroup_;
    mutable std::mutex mu_;

    struct TrackedApp {
        std::string app_name;
        pid_t pid{0};
        ResourceQuota quota;
        AppUsage last_usage;
        uint64_t prev_cpu_usec{0};         // for delta calculation
        std::chrono::steady_clock::time_point prev_sample;
        int warn_count{0};
        int throttle_count{0};
    };

    std::unordered_map<std::string, TrackedApp> tracked_;
    std::vector<Violation> violations_;
    bool enforcement_enabled_{true};

    /** Sample current usage for a tracked app. */
    AppUsage sample_usage(TrackedApp& app);

    /** Read VRAM usage from VPU sysfs. */
    uint64_t read_vram_usage(pid_t pid);

    /** Read GPU compute usage from sysfs. */
    double read_gpu_usage(pid_t pid);

    /** Read compositor FPS from D-Bus or sysfs. */
    double read_compositor_fps(pid_t pid);

    /** Determine enforcement action based on usage vs quota. */
    EnforcementAction determine_action(const AppUsage& usage,
                                        const ResourceQuota& quota,
                                        const EnforcementPolicy& policy);

    /** Execute an enforcement action. */
    void execute_action(TrackedApp& app, EnforcementAction action);

    /** Record a violation. */
    void record_violation(const std::string& app, const std::string& resource,
                          double usage_pct, EnforcementAction action);

    /** Current ISO 8601 timestamp. */
    static std::string now_iso8601();
};

} // namespace straylight::quota
