/**
 * StrayLight Throttle Controller — Pre-emptive thermal throttling.
 *
 * Evaluates thermal predictions and applies throttle actions BEFORE
 * hardware thermal limits are reached. Includes hysteresis to prevent
 * oscillation: requires temperature to drop hysteresis degrees below
 * the throttle threshold before un-throttling.
 */
#pragma once

#include "thermal_model.h"
#include "straylight/result.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace straylight::thermal {

/// Individual throttle action types.
enum class ThrottleActionType {
    ReduceGpuClocks,
    LowerCpuFreq,
    ReduceCompositorFps,
    PauseMeshJobs,
    ReduceVpuSlabs
};

inline const char* action_type_to_string(ThrottleActionType a) {
    switch (a) {
        case ThrottleActionType::ReduceGpuClocks:     return "reduce_gpu_clocks";
        case ThrottleActionType::LowerCpuFreq:        return "lower_cpu_freq";
        case ThrottleActionType::ReduceCompositorFps:  return "reduce_compositor_fps";
        case ThrottleActionType::PauseMeshJobs:        return "pause_mesh_jobs";
        case ThrottleActionType::ReduceVpuSlabs:       return "reduce_vpu_slabs";
    }
    return "unknown";
}

/// A single throttle action with its current state.
struct ThrottleAction {
    ThrottleActionType type;
    bool active = false;
    std::chrono::steady_clock::time_point activated_at;
    std::string description;
};

/// Throttling policy determines aggressiveness.
enum class ThrottlePolicy {
    Aggressive,     // Throttle early (10C before threshold)
    Balanced,       // Throttle at threshold
    Relaxed         // Throttle 5C above threshold
};

inline const char* policy_to_string(ThrottlePolicy p) {
    switch (p) {
        case ThrottlePolicy::Aggressive: return "aggressive";
        case ThrottlePolicy::Balanced:   return "balanced";
        case ThrottlePolicy::Relaxed:    return "relaxed";
    }
    return "unknown";
}

inline ThrottlePolicy policy_from_string(const std::string& s) {
    if (s == "aggressive") return ThrottlePolicy::Aggressive;
    if (s == "relaxed")    return ThrottlePolicy::Relaxed;
    return ThrottlePolicy::Balanced;
}

class ThrottleController {
public:
    ThrottleController();

    /// Evaluate all zones and apply/remove throttle actions as needed.
    VoidResult<std::string> evaluate_and_act(
        const ThermalModel& model,
        const ThermalConfig& config);

    /// Set the throttle policy.
    void set_policy(ThrottlePolicy policy) { policy_ = policy; }
    [[nodiscard]] ThrottlePolicy policy() const { return policy_; }

    /// Get currently active actions.
    [[nodiscard]] const std::vector<ThrottleAction>& actions() const { return actions_; }

    /// Check if any throttle is currently active.
    [[nodiscard]] bool is_throttled() const;

    /// Get count of active throttle actions.
    [[nodiscard]] int active_count() const;

private:
    std::vector<ThrottleAction> actions_;
    ThrottlePolicy policy_ = ThrottlePolicy::Balanced;
    bool throttle_engaged_ = false;
    int throttle_engaged_max_temp_ = 0;

    /// Apply a specific throttle action.
    void apply_action(ThrottleAction& action);

    /// Remove a specific throttle action.
    void remove_action(ThrottleAction& action);

    /// Get the effective throttle temperature based on policy.
    int effective_throttle_temp(const ThermalConfig& config) const;

    /// Write to CPU scaling governor.
    static void set_cpu_governor(const std::string& governor);

    /// Set GPU clock limits.
    static void set_gpu_clock_limit(int mhz);

    /// Notify compositor to reduce FPS.
    static void notify_compositor_fps(int fps);

    /// Pause mesh network background jobs.
    static void pause_mesh_jobs(bool pause);

    /// Reduce VPU slab allocations.
    static void reduce_vpu_slabs(bool reduce);
};

} // namespace straylight::thermal
