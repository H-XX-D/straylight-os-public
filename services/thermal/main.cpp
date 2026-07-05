/**
 * straylight-thermal — Predictive thermal management daemon.
 *
 * Polls thermal sensors, predicts near-term temperature, and applies
 * pre-emptive throttling before hardware limits are reached.
 */

#include "thermal_model.h"
#include "throttle_controller.h"
#include "thermal_log.h"

#include <straylight/config.h>
#include <straylight/daemon.h>
#include <straylight/log.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>

namespace straylight::thermal {

class ThermalDaemon : public DaemonBase {
public:
    Result<void, SLError> init(const Config& /*cfg*/) override {
        const std::string cfg_path = "/etc/straylight/thermal.conf";

        auto cfg_result = ThermalConfig::load(cfg_path);
        if (cfg_result.has_value()) {
            config_ = cfg_result.value();
            SL_INFO("thermal: loaded config from {}", cfg_path);
        } else {
            SL_WARN("thermal: config load failed ({}), using defaults",
                    cfg_result.error());
        }

        config_.poll_interval_ms = std::max(250, config_.poll_interval_ms);

        auto log_result = log_.init();
        if (!log_result.has_value()) {
            SL_WARN("thermal: log init warning: {}", log_result.error());
        }

        auto discover_result = model_.discover_zones();
        if (!discover_result.has_value()) {
            return Result<void, SLError>::error(
                SLError{SLErrorCode::Internal,
                        "zone discovery failed: " + discover_result.error()});
        }

        SL_INFO("thermal: discovered {} thermal zones", model_.zones().size());
        for (const auto& z : model_.zones()) {
            SL_INFO("thermal: zone {} ({}) = {}C", z.name, z.type, z.current_temp);
        }

        return Result<void, SLError>::ok();
    }

    Result<void, SLError> tick() override {
        auto poll_result = model_.poll();
        if (!poll_result.has_value()) {
            SL_WARN("thermal: poll error: {}", poll_result.error());
            sleep_interval();
            return Result<void, SLError>::ok();
        }

        if (config_.enable_prediction) {
            for (auto& zone : model_.zones()) {
                zone.predicted_temp_5s =
                    model_.predict_temperature(zone, config_.prediction_horizon_s);
            }
        }

        bool was_throttled = throttle_.is_throttled();
        auto throttle_result = throttle_.evaluate_and_act(model_, config_);
        if (!throttle_result.has_value()) {
            SL_WARN("thermal: throttle error: {}", throttle_result.error());
        }

        if (throttle_.is_throttled() != was_throttled) {
            int max_temp = 0;
            double max_pred = 0.0;
            for (const auto& z : model_.zones()) {
                max_temp = std::max(max_temp, z.current_temp);
                max_pred = std::max(max_pred, z.predicted_temp_5s);
            }
            log_.log_throttle_change(
                throttle_.is_throttled() ? "engaged" : "released",
                max_temp,
                max_pred);
        }

        log_.log_poll(model_, config_, throttle_);

        ++tick_count_;
        if (tick_count_ % 10 == 0) {
            log_.flush();
        }

        sleep_interval();
        return Result<void, SLError>::ok();
    }

    void shutdown() override {
        if (throttle_.is_throttled()) {
            SL_INFO("thermal: releasing throttles on shutdown");
            ThermalConfig cool_cfg = config_;
            cool_cfg.throttle_temp = 999;
            cool_cfg.critical_temp = 999;
            (void)throttle_.evaluate_and_act(model_, cool_cfg);
        }

        log_.log_throttle_change("daemon_shutdown", 0, 0.0);
        log_.flush();
    }

private:
    void sleep_interval() const {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(config_.poll_interval_ms));
    }

    ThermalConfig config_;
    ThermalModel model_;
    ThrottleController throttle_;
    ThermalLog log_;
    uint64_t tick_count_ = 0;
};

} // namespace straylight::thermal

int main(int /*argc*/, char** /*argv*/) {
    straylight::Log::init("straylight-thermal");
    straylight::thermal::ThermalDaemon daemon;
    return daemon.run(straylight::Config::make_empty());
}
