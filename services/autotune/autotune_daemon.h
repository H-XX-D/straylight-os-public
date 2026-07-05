// services/autotune/autotune_daemon.h
#pragma once

#include <straylight/common.h>
#include <straylight/daemon.h>
#include "tuner.h"

#include <chrono>
#include <mutex>

namespace straylight {

/// Daemon that runs the SystemTuner on a periodic schedule and exposes
/// D-Bus interface org.straylight.Autotune1 for runtime profile switching.
class AutotuneDaemon : public DaemonBase {
public:
    Result<void, SLError> init(const Config& cfg) override;
    Result<void, SLError> tick() override;
    void shutdown() override;

    /// D-Bus method handlers (called from the D-Bus dispatch thread in production).
    void dbus_set_profile(const std::string& name);
    std::string dbus_get_profile() const;
    std::vector<std::string> dbus_list_profiles() const;
    void dbus_retune();

private:
    SystemTuner tuner_;
    bool auto_detect_   = true;
    bool tune_on_boot_  = true;
    int periodic_check_s_ = 300;
    bool initial_tune_done_ = false;

    std::chrono::steady_clock::time_point last_tune_time_;
    mutable std::mutex mutex_;
};

} // namespace straylight
