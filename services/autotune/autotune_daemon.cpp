// services/autotune/autotune_daemon.cpp
#include "autotune_daemon.h"
#include <thread>
#include <chrono>

namespace straylight {

Result<void, SLError> AutotuneDaemon::init(const Config& cfg) {
    // Load config
    auto profile_name = cfg.get<std::string>("profile", "balanced");
    auto_detect_      = cfg.get<bool>("auto_detect", true);
    tune_on_boot_     = cfg.get<bool>("tune_on_boot", true);
    periodic_check_s_ = cfg.get<int>("periodic_check_seconds", 300);

    // Feature flags
    tuner_.features().cpu     = cfg.get<bool>("features.cpu", true);
    tuner_.features().memory  = cfg.get<bool>("features.memory", true);
    tuner_.features().io      = cfg.get<bool>("features.io", true);
    tuner_.features().network = cfg.get<bool>("features.network", true);
    tuner_.features().gpu     = cfg.get<bool>("features.gpu", true);
    tuner_.features().kernel  = cfg.get<bool>("features.kernel", true);

    tuner_.set_profile(profile_name);

    SL_INFO("autotune: initialized (profile={}, auto_detect={}, tune_on_boot={}, period={}s)",
            profile_name, auto_detect_, tune_on_boot_, periodic_check_s_);

    // Run initial tune if configured
    if (tune_on_boot_) {
        auto r = tuner_.detect_and_tune();
        if (!r.has_value()) {
            SL_WARN("autotune: initial tune failed: {}", r.error());
            // Non-fatal — the daemon keeps running and will retry on next tick
        }
        initial_tune_done_ = true;
        last_tune_time_ = std::chrono::steady_clock::now();
    }

    return Result<void, SLError>::ok();
}

Result<void, SLError> AutotuneDaemon::tick() {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_tune_time_).count();

    // Periodic re-tune
    if (periodic_check_s_ > 0 && elapsed >= periodic_check_s_) {
        SL_INFO("autotune: periodic check ({}s elapsed)", elapsed);
        auto r = tuner_.detect_and_tune();
        if (!r.has_value()) {
            SL_WARN("autotune: periodic tune failed: {}", r.error());
        }
        last_tune_time_ = now;
    }

    // Sleep to avoid busy-spinning. We use 5s granularity to stay responsive
    // to shutdown signals while not wasting CPU on constant polling.
    std::this_thread::sleep_for(std::chrono::seconds(5));
    return Result<void, SLError>::ok();
}

void AutotuneDaemon::shutdown() {
    SL_INFO("autotune: shutting down");
}

// ---------------------------------------------------------------------------
// D-Bus method handlers
// ---------------------------------------------------------------------------

void AutotuneDaemon::dbus_set_profile(const std::string& name) {
    std::lock_guard lock(mutex_);
    tuner_.set_profile(name);
    // Apply immediately
    auto r = tuner_.detect_and_tune();
    if (!r.has_value()) {
        SL_WARN("autotune: retune after profile switch failed: {}", r.error());
    }
    last_tune_time_ = std::chrono::steady_clock::now();
}

std::string AutotuneDaemon::dbus_get_profile() const {
    std::lock_guard lock(mutex_);
    return tuner_.current_profile();
}

std::vector<std::string> AutotuneDaemon::dbus_list_profiles() const {
    std::lock_guard lock(mutex_);
    return tuner_.available_profiles();
}

void AutotuneDaemon::dbus_retune() {
    std::lock_guard lock(mutex_);
    auto r = tuner_.detect_and_tune();
    if (!r.has_value()) {
        SL_WARN("autotune: manual retune failed: {}", r.error());
    }
    last_tune_time_ = std::chrono::steady_clock::now();
}

} // namespace straylight
