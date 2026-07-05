// services/power/main.cpp
// straylight-power — Power management daemon for StrayLight OS.
#include "power_engine.h"

#include <straylight/common.h>
#include <straylight/daemon.h>

#include <chrono>
#include <thread>

namespace straylight {

/// Power management daemon: monitors battery, handles profiles, lid switch.
class PowerDaemon : public DaemonBase {
public:
    Result<void, SLError> init(const Config& cfg) override {
        poll_interval_s_ = cfg.get<int>("poll_interval_s", 30);
        auto_profile_ = cfg.get<bool>("auto_profile", true);
        warn_percent_ = cfg.get<int>("warn_percent", 20);
        hibernate_percent_ = cfg.get<int>("hibernate_percent", 5);

        // Set initial profile based on power source.
        if (auto_profile_) {
            auto source = engine_.get_power_source();
            if (source == PowerSource::AC) {
                engine_.set_profile(PowerProfile::Performance);
                SL_INFO("power: AC detected, setting performance profile");
            } else {
                engine_.set_profile(PowerProfile::Powersave);
                SL_INFO("power: battery detected, setting powersave profile");
            }
        }

        SL_INFO("power: daemon initialized (poll={}s, auto_profile={})",
                poll_interval_s_, auto_profile_);
        return Result<void, SLError>::ok();
    }

    Result<void, SLError> tick() override {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_poll_).count();

        if (elapsed < poll_interval_s_) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            return Result<void, SLError>::ok();
        }

        last_poll_ = now;

        // Check battery status.
        auto bat_result = engine_.get_primary_battery();
        if (bat_result.has_value()) {
            const auto& bat = bat_result.value();

            // Low battery actions.
            engine_.check_low_battery(bat);

            // Auto-switch profiles on AC/battery transitions.
            if (auto_profile_) {
                auto source = engine_.get_power_source();
                if (source != last_source_) {
                    if (source == PowerSource::AC) {
                        engine_.set_profile(PowerProfile::Performance);
                        SL_INFO("power: switched to AC, profile -> performance");
                    } else {
                        engine_.set_profile(PowerProfile::Powersave);
                        SL_INFO("power: switched to battery, profile -> powersave");
                    }
                    last_source_ = source;
                }
            }

            // Log battery state periodically.
            SL_DEBUG("power: battery {}% ({}) {}W",
                     bat.capacity_percent,
                     (bat.status == BatteryStatus::Charging ? "charging" :
                      bat.status == BatteryStatus::Discharging ? "discharging" : "idle"),
                     bat.power_now_uw / 1000000.0);
        }

        return Result<void, SLError>::ok();
    }

    void shutdown() override {
        SL_INFO("power: daemon shutting down");
    }

private:
    PowerEngine engine_;
    int poll_interval_s_ = 30;
    bool auto_profile_ = true;
    int warn_percent_ = 20;
    int hibernate_percent_ = 5;

    PowerSource last_source_ = PowerSource::Unknown;
    std::chrono::steady_clock::time_point last_poll_;
};

} // namespace straylight

int main(int /*argc*/, char* /*argv*/[]) {
    straylight::Log::init("straylight-power");

    auto cfg_result = straylight::Config::load("/etc/straylight/power.conf");
    if (!cfg_result.has_value()) {
        SL_WARN("power: no config found, using defaults");
        auto default_cfg = straylight::Config::load("/dev/null");
        if (!default_cfg.has_value()) {
            SL_ERROR("power: cannot create default config");
            return 1;
        }
        straylight::PowerDaemon daemon;
        return daemon.run(default_cfg.value());
    }

    straylight::PowerDaemon daemon;
    return daemon.run(cfg_result.value());
}
