/**
 * StrayLight Throttle Controller — Implementation.
 *
 * Pre-emptive throttling based on predicted temperatures.
 * Hysteresis prevents oscillation: must drop hysteresis degrees
 * below threshold before un-throttling.
 */

#include "throttle_controller.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace straylight::thermal {

ThrottleController::ThrottleController() {
    // Initialize all possible throttle actions.
    actions_ = {
        {ThrottleActionType::ReduceGpuClocks,    false, {}, "Reduce GPU core/memory clocks to 70%"},
        {ThrottleActionType::LowerCpuFreq,       false, {}, "Switch CPU governor to powersave"},
        {ThrottleActionType::ReduceCompositorFps, false, {}, "Reduce compositor to 30 FPS"},
        {ThrottleActionType::PauseMeshJobs,       false, {}, "Pause non-critical mesh network jobs"},
        {ThrottleActionType::ReduceVpuSlabs,      false, {}, "Reduce VPU slab allocation rate"},
    };
}

int ThrottleController::effective_throttle_temp(const ThermalConfig& config) const {
    switch (policy_) {
        case ThrottlePolicy::Aggressive:
            return config.throttle_temp - 10;
        case ThrottlePolicy::Relaxed:
            return config.throttle_temp + 5;
        case ThrottlePolicy::Balanced:
        default:
            return config.throttle_temp;
    }
}

VoidResult<std::string> ThrottleController::evaluate_and_act(
    const ThermalModel& model,
    const ThermalConfig& config)
{
    int eff_throttle = effective_throttle_temp(config);
    int max_current = 0;
    double max_predicted = 0.0;

    for (const auto& zone : model.zones()) {
        if (zone.current_temp > max_current) {
            max_current = zone.current_temp;
        }

        double pred = model.predict_temperature(zone, config.prediction_horizon_s);
        if (pred > max_predicted) {
            max_predicted = pred;
        }
    }

    bool should_throttle = false;

    // Check if predicted temperature will exceed threshold.
    if (config.enable_prediction && max_predicted >= static_cast<double>(eff_throttle)) {
        should_throttle = true;
    }

    // Check if current temperature already exceeds threshold.
    if (max_current >= eff_throttle) {
        should_throttle = true;
    }

    // Hysteresis: once engaged, don't release until temp drops hysteresis degrees
    // below the throttle threshold.
    if (throttle_engaged_) {
        int release_threshold = eff_throttle - config.hysteresis;
        if (max_current < release_threshold &&
            max_predicted < static_cast<double>(release_threshold)) {
            // Temperature has dropped sufficiently — release throttle.
            should_throttle = false;
        } else {
            // Still above release threshold — keep throttling.
            should_throttle = true;
        }
    }

    if (should_throttle && !throttle_engaged_) {
        // Engage throttling: activate all actions progressively based on severity.
        throttle_engaged_ = true;
        throttle_engaged_max_temp_ = max_current;

        fprintf(stdout, "[thermal] engaging throttle: current=%dC predicted=%.1fC threshold=%dC\n",
                max_current, max_predicted, eff_throttle);

        // Always apply CPU and compositor throttle first.
        for (auto& action : actions_) {
            if (action.type == ThrottleActionType::LowerCpuFreq ||
                action.type == ThrottleActionType::ReduceCompositorFps) {
                apply_action(action);
            }
        }

        // If approaching critical, apply all.
        if (max_current >= config.critical_temp - 10 ||
            max_predicted >= static_cast<double>(config.critical_temp - 5)) {
            for (auto& action : actions_) {
                if (!action.active) {
                    apply_action(action);
                }
            }
        }
    } else if (should_throttle && throttle_engaged_) {
        // Escalate if temperature is still rising.
        if (max_current > throttle_engaged_max_temp_) {
            throttle_engaged_max_temp_ = max_current;
            for (auto& action : actions_) {
                if (!action.active) {
                    apply_action(action);
                    fprintf(stdout, "[thermal] escalating: activating %s (temp=%dC)\n",
                            action_type_to_string(action.type), max_current);
                }
            }
        }
    } else if (!should_throttle && throttle_engaged_) {
        // Disengage all throttle actions.
        throttle_engaged_ = false;
        throttle_engaged_max_temp_ = 0;

        fprintf(stdout, "[thermal] releasing throttle: current=%dC predicted=%.1fC\n",
                max_current, max_predicted);

        for (auto& action : actions_) {
            if (action.active) {
                remove_action(action);
            }
        }
    }

    return VoidResult<std::string>::ok();
}

bool ThrottleController::is_throttled() const {
    return throttle_engaged_;
}

int ThrottleController::active_count() const {
    int count = 0;
    for (const auto& a : actions_) {
        if (a.active) ++count;
    }
    return count;
}

void ThrottleController::apply_action(ThrottleAction& action) {
    if (action.active) return;

    action.active = true;
    action.activated_at = std::chrono::steady_clock::now();

    switch (action.type) {
        case ThrottleActionType::LowerCpuFreq:
            set_cpu_governor("powersave");
            break;
        case ThrottleActionType::ReduceGpuClocks:
            set_gpu_clock_limit(700); // 700 MHz cap
            break;
        case ThrottleActionType::ReduceCompositorFps:
            notify_compositor_fps(30);
            break;
        case ThrottleActionType::PauseMeshJobs:
            pause_mesh_jobs(true);
            break;
        case ThrottleActionType::ReduceVpuSlabs:
            reduce_vpu_slabs(true);
            break;
    }

    fprintf(stdout, "[thermal] applied throttle action: %s\n",
            action_type_to_string(action.type));
}

void ThrottleController::remove_action(ThrottleAction& action) {
    if (!action.active) return;
    action.active = false;

    switch (action.type) {
        case ThrottleActionType::LowerCpuFreq:
            set_cpu_governor("schedutil");
            break;
        case ThrottleActionType::ReduceGpuClocks:
            set_gpu_clock_limit(0); // 0 = remove limit
            break;
        case ThrottleActionType::ReduceCompositorFps:
            notify_compositor_fps(60);
            break;
        case ThrottleActionType::PauseMeshJobs:
            pause_mesh_jobs(false);
            break;
        case ThrottleActionType::ReduceVpuSlabs:
            reduce_vpu_slabs(false);
            break;
    }

    fprintf(stdout, "[thermal] removed throttle action: %s\n",
            action_type_to_string(action.type));
}

// ---------------------------------------------------------------------------
// System control primitives
// ---------------------------------------------------------------------------

void ThrottleController::set_cpu_governor(const std::string& governor) {
    namespace fs = std::filesystem;
    const std::string cpu_base = "/sys/devices/system/cpu";

    if (!fs::exists(cpu_base)) return;

    std::error_code ec;
    for (auto& entry : fs::directory_iterator(cpu_base, ec)) {
        std::string name = entry.path().filename().string();
        if (name.rfind("cpu", 0) != 0) continue;
        // Skip non-numeric cpu entries (cpufreq, cpuidle, etc.).
        if (name.size() < 4 || !std::isdigit(name[3])) continue;

        std::string gov_path = entry.path().string() + "/cpufreq/scaling_governor";
        if (fs::exists(gov_path)) {
            std::ofstream gf(gov_path);
            if (gf) gf << governor;
        }
    }
}

void ThrottleController::set_gpu_clock_limit(int mhz) {
    // NVIDIA: write to /sys/class/drm/card0/device/power/max_clock
    // or use nvidia-smi.
    namespace fs = std::filesystem;

    // Try sysfs first.
    const std::string nvidia_power = "/sys/class/drm/card0/device/power";
    if (fs::exists(nvidia_power)) {
        std::string max_clock_path = nvidia_power + "/max_clock";
        if (fs::exists(max_clock_path)) {
            std::ofstream cf(max_clock_path);
            if (cf) {
                if (mhz > 0) {
                    cf << mhz;
                } else {
                    cf << "auto";
                }
            }
            return;
        }
    }

    // Fallback: nvidia-smi persistence mode + clock lock.
    if (mhz > 0) {
        std::string cmd = "nvidia-smi -lgc 0," + std::to_string(mhz) + " 2>/dev/null";
        (void)system(cmd.c_str());
    } else {
        (void)system("nvidia-smi -rgc 2>/dev/null");
    }
}

void ThrottleController::notify_compositor_fps(int fps) {
    // Write to StrayLight compositor control socket/file.
    const std::string fps_path = "/run/straylight/compositor/target_fps";
    namespace fs = std::filesystem;

    std::error_code ec;
    fs::create_directories("/run/straylight/compositor", ec);

    std::ofstream ff(fps_path);
    if (ff) {
        ff << fps;
    }
}

void ThrottleController::pause_mesh_jobs(bool pause) {
    // Signal the mesh daemon via its control file.
    const std::string mesh_ctl = "/run/straylight/mesh/pause_jobs";
    namespace fs = std::filesystem;

    std::error_code ec;
    fs::create_directories("/run/straylight/mesh", ec);

    std::ofstream mf(mesh_ctl);
    if (mf) {
        mf << (pause ? "1" : "0");
    }
}

void ThrottleController::reduce_vpu_slabs(bool reduce) {
    // Write to VPU slab controller.
    const std::string vpu_ctl = "/sys/kernel/straylight-vpu/slab_reduce";
    std::ofstream vf(vpu_ctl);
    if (vf) {
        vf << (reduce ? "1" : "0");
    }
}

} // namespace straylight::thermal
