/**
 * StrayLight Thermal Model — Unified thermal sensor abstraction.
 *
 * Reads thermal data from:
 *   - thermal zones under /sys/class/thermal/
 *   - hwmon devices under /sys/class/hwmon/
 *   - /sys/kernel/straylight-vpu/memory_pressure (VPU thermals)
 *   - nvidia-smi / sysfs GPU temperature
 *
 * Provides linear-regression based temperature prediction and
 * aggregated thermal state across all zones.
 */
#pragma once

#include "straylight/result.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <numeric>
#include <string>
#include <vector>

namespace straylight::thermal {

/// Thermal trend direction.
enum class ThermalTrend {
    Rising,
    Falling,
    Stable
};

inline const char* trend_to_string(ThermalTrend t) {
    switch (t) {
        case ThermalTrend::Rising:  return "rising";
        case ThermalTrend::Falling: return "falling";
        case ThermalTrend::Stable:  return "stable";
    }
    return "unknown";
}

/// Aggregated system thermal state.
enum class ThermalState {
    Cool,
    Warm,
    Hot,
    Critical
};

inline const char* state_to_string(ThermalState s) {
    switch (s) {
        case ThermalState::Cool:     return "cool";
        case ThermalState::Warm:     return "warm";
        case ThermalState::Hot:      return "hot";
        case ThermalState::Critical: return "critical";
    }
    return "unknown";
}

/// A single trip point threshold.
struct TripPoint {
    int temperature;    // degrees C
    std::string type;   // "passive", "active", "critical", "hot"
};

/// Represents one thermal zone with history and prediction.
struct ThermalZone {
    std::string name;
    std::string type;               // e.g., "x86_pkg_temp", "acpitz", "vpu", "gpu"
    std::string sysfs_path;         // e.g., "/sys/class/thermal/thermal_zone0"
    int current_temp = 0;           // millidegrees C from sysfs, converted to degrees C
    std::vector<TripPoint> trip_points;
    ThermalTrend trend = ThermalTrend::Stable;
    double predicted_temp_5s = 0.0;

    /// Ring buffer of recent temperature samples (up to 30).
    std::deque<double> history;
    static constexpr size_t kMaxHistory = 30;

    void record_sample(double temp_c) {
        current_temp = static_cast<int>(temp_c);
        history.push_back(temp_c);
        if (history.size() > kMaxHistory) {
            history.pop_front();
        }
    }
};

/// Configuration loaded from thermal.conf.
struct ThermalConfig {
    int poll_interval_ms = 1000;
    int warn_temp = 75;
    int throttle_temp = 85;
    int critical_temp = 95;
    int hysteresis = 5;
    bool enable_prediction = true;
    int prediction_horizon_s = 5;

    static Result<ThermalConfig, std::string> load(const std::string& path);
};

/// The unified thermal model.
class ThermalModel {
public:
    ThermalModel() = default;

    /// Discover all thermal zones from sysfs, hwmon, VPU, GPU.
    VoidResult<std::string> discover_zones();

    /// Poll all zones, update temperatures and trends.
    VoidResult<std::string> poll();

    /// Predict temperature for a given zone at horizon_seconds into the future.
    /// Uses linear regression over the last 30 samples.
    double predict_temperature(const ThermalZone& zone, int horizon_seconds) const;

    /// Get the overall thermal state across all zones.
    ThermalState get_overall_thermal_state(const ThermalConfig& config) const;

    /// Access discovered zones.
    [[nodiscard]] const std::vector<ThermalZone>& zones() const { return zones_; }
    [[nodiscard]] std::vector<ThermalZone>& zones() { return zones_; }

    /// Find a zone by name.
    ThermalZone* find_zone(const std::string& name);
    const ThermalZone* find_zone(const std::string& name) const;

    /// Update trend for a zone based on its history.
    static ThermalTrend compute_trend(const ThermalZone& zone);

private:
    std::vector<ThermalZone> zones_;

    /// Read temperature from sysfs path (returns degrees C).
    static double read_sysfs_temp(const std::string& path);

    /// Read trip points from a thermal zone sysfs directory.
    static std::vector<TripPoint> read_trip_points(const std::string& zone_path);

    /// Discover zones from /sys/class/thermal/.
    void discover_thermal_zones();

    /// Discover zones from /sys/class/hwmon/.
    void discover_hwmon_zones();

    /// Discover VPU thermal zone from StrayLight VPU sysfs.
    void discover_vpu_zone();

    /// Discover GPU thermal zone from nvidia sysfs or nvidia-smi.
    void discover_gpu_zone();

    /// Linear regression: returns (slope, intercept) over the sample history.
    static std::pair<double, double> linear_regression(const std::deque<double>& samples);
};

} // namespace straylight::thermal
