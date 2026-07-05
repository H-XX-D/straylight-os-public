/**
 * StrayLight Thermal Model — Implementation.
 *
 * Reads from sysfs thermal zones, hwmon, VPU memory pressure sysfs,
 * and nvidia GPU sysfs. Provides linear regression prediction and
 * unified thermal state aggregation.
 */

#include "thermal_model.h"

#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <sstream>

namespace straylight::thermal {

// ---------------------------------------------------------------------------
// ThermalConfig
// ---------------------------------------------------------------------------

Result<ThermalConfig, std::string> ThermalConfig::load(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return Result<ThermalConfig, std::string>::error(
            "cannot open config file: " + path);
    }

    ThermalConfig cfg;
    std::string line;
    while (std::getline(file, line)) {
        // Strip comments and leading/trailing whitespace.
        auto comment_pos = line.find('#');
        if (comment_pos != std::string::npos) {
            line = line.substr(0, comment_pos);
        }

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        // Trim whitespace.
        auto trim = [](std::string& s) {
            size_t start = s.find_first_not_of(" \t\r\n");
            size_t end = s.find_last_not_of(" \t\r\n");
            if (start == std::string::npos) { s.clear(); return; }
            s = s.substr(start, end - start + 1);
        };
        trim(key);
        trim(val);

        if (key == "poll_interval_ms")    cfg.poll_interval_ms = std::stoi(val);
        else if (key == "warn_temp")      cfg.warn_temp = std::stoi(val);
        else if (key == "throttle_temp")  cfg.throttle_temp = std::stoi(val);
        else if (key == "critical_temp")  cfg.critical_temp = std::stoi(val);
        else if (key == "hysteresis")     cfg.hysteresis = std::stoi(val);
        else if (key == "enable_prediction") cfg.enable_prediction = (val == "true" || val == "1");
        else if (key == "prediction_horizon_s") cfg.prediction_horizon_s = std::stoi(val);
    }

    return Result<ThermalConfig, std::string>::ok(cfg);
}

// ---------------------------------------------------------------------------
// Sysfs helpers
// ---------------------------------------------------------------------------

double ThermalModel::read_sysfs_temp(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return 0.0;
    int millideg = 0;
    f >> millideg;
    return static_cast<double>(millideg) / 1000.0;
}

std::vector<TripPoint> ThermalModel::read_trip_points(const std::string& zone_path) {
    std::vector<TripPoint> points;
    namespace fs = std::filesystem;

    for (int i = 0; i < 20; ++i) {
        std::string temp_path = zone_path + "/trip_point_" + std::to_string(i) + "_temp";
        std::string type_path = zone_path + "/trip_point_" + std::to_string(i) + "_type";

        if (!fs::exists(temp_path)) break;

        TripPoint tp;

        std::ifstream tf(temp_path);
        if (tf) {
            int millideg = 0;
            tf >> millideg;
            tp.temperature = millideg / 1000;
        }

        std::ifstream tyf(type_path);
        if (tyf) {
            std::getline(tyf, tp.type);
            // Trim newline.
            while (!tp.type.empty() && (tp.type.back() == '\n' || tp.type.back() == '\r')) {
                tp.type.pop_back();
            }
        }

        points.push_back(std::move(tp));
    }

    return points;
}

// ---------------------------------------------------------------------------
// Zone discovery
// ---------------------------------------------------------------------------

void ThermalModel::discover_thermal_zones() {
    namespace fs = std::filesystem;
    const std::string base = "/sys/class/thermal";

    if (!fs::exists(base)) return;

    std::error_code ec;
    for (auto& entry : fs::directory_iterator(base, ec)) {
        std::string dirname = entry.path().filename().string();
        if (dirname.rfind("thermal_zone", 0) != 0) continue;

        ThermalZone zone;
        zone.sysfs_path = entry.path().string();
        zone.name = dirname;

        // Read zone type.
        std::ifstream type_file(zone.sysfs_path + "/type");
        if (type_file) {
            std::getline(type_file, zone.type);
            while (!zone.type.empty() && (zone.type.back() == '\n' || zone.type.back() == '\r')) {
                zone.type.pop_back();
            }
        }

        // Read trip points.
        zone.trip_points = read_trip_points(zone.sysfs_path);

        // Initial temperature reading.
        double temp = read_sysfs_temp(zone.sysfs_path + "/temp");
        zone.record_sample(temp);

        zones_.push_back(std::move(zone));
    }
}

void ThermalModel::discover_hwmon_zones() {
    namespace fs = std::filesystem;
    const std::string base = "/sys/class/hwmon";

    if (!fs::exists(base)) return;

    std::error_code ec;
    for (auto& entry : fs::directory_iterator(base, ec)) {
        std::string hwmon_path = entry.path().string();

        // Read the name of this hwmon device.
        std::string hw_name;
        std::ifstream nf(hwmon_path + "/name");
        if (nf) {
            std::getline(nf, hw_name);
            while (!hw_name.empty() && (hw_name.back() == '\n' || hw_name.back() == '\r')) {
                hw_name.pop_back();
            }
        }

        // Look for temp*_input files.
        for (int i = 1; i <= 16; ++i) {
            std::string temp_path = hwmon_path + "/temp" + std::to_string(i) + "_input";
            if (!fs::exists(temp_path)) continue;

            // Check if we already have this zone via thermal_zone (avoid duplicates).
            std::string zname = hw_name + "_temp" + std::to_string(i);
            bool duplicate = false;
            for (const auto& z : zones_) {
                if (z.type == hw_name) { duplicate = true; break; }
            }
            if (duplicate) continue;

            ThermalZone zone;
            zone.sysfs_path = temp_path;
            zone.name = zname;
            zone.type = hw_name;

            // Read critical trip from hwmon if available.
            std::string crit_path = hwmon_path + "/temp" + std::to_string(i) + "_crit";
            if (fs::exists(crit_path)) {
                TripPoint tp;
                std::ifstream cf(crit_path);
                int millideg = 0;
                cf >> millideg;
                tp.temperature = millideg / 1000;
                tp.type = "critical";
                zone.trip_points.push_back(tp);
            }

            double temp = read_sysfs_temp(temp_path);
            zone.record_sample(temp);

            zones_.push_back(std::move(zone));
        }
    }
}

void ThermalModel::discover_vpu_zone() {
    namespace fs = std::filesystem;
    const std::string vpu_pressure = "/sys/kernel/straylight-vpu/memory_pressure";
    const std::string vpu_temp = "/sys/kernel/straylight-vpu/temperature";

    // VPU may expose temperature directly or we derive from memory pressure.
    ThermalZone zone;
    zone.name = "vpu";
    zone.type = "straylight-vpu";

    if (fs::exists(vpu_temp)) {
        zone.sysfs_path = vpu_temp;
        double temp = read_sysfs_temp(vpu_temp);
        zone.record_sample(temp);
        zones_.push_back(std::move(zone));
    } else if (fs::exists(vpu_pressure)) {
        // Estimate temperature from memory pressure (0-100 scale maps ~40-100C).
        zone.sysfs_path = vpu_pressure;
        std::ifstream pf(vpu_pressure);
        int pressure = 0;
        if (pf) pf >> pressure;
        double estimated_temp = 40.0 + (pressure * 0.6);
        zone.record_sample(estimated_temp);
        zones_.push_back(std::move(zone));
    }
    // If neither exists, VPU is not present — skip silently.
}

void ThermalModel::discover_gpu_zone() {
    namespace fs = std::filesystem;

    // Try NVIDIA sysfs first (newer drivers).
    const std::string nvidia_sysfs = "/sys/class/hwmon";
    bool found_nvidia = false;

    if (fs::exists(nvidia_sysfs)) {
        std::error_code ec;
        for (auto& entry : fs::directory_iterator(nvidia_sysfs, ec)) {
            std::string name_path = entry.path().string() + "/name";
            std::ifstream nf(name_path);
            std::string name;
            if (nf) std::getline(nf, name);
            // Trim.
            while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) {
                name.pop_back();
            }

            if (name == "nvidia" || name.find("gpu") != std::string::npos) {
                std::string temp_path = entry.path().string() + "/temp1_input";
                if (fs::exists(temp_path)) {
                    ThermalZone zone;
                    zone.name = "gpu";
                    zone.type = "nvidia-gpu";
                    zone.sysfs_path = temp_path;
                    double temp = read_sysfs_temp(temp_path);
                    zone.record_sample(temp);
                    zones_.push_back(std::move(zone));
                    found_nvidia = true;
                    break;
                }
            }
        }
    }

    // Fallback: try nvidia-smi.
    if (!found_nvidia) {
        FILE* pipe = popen("nvidia-smi --query-gpu=temperature.gpu --format=csv,noheader,nounits 2>/dev/null", "r");
        if (pipe) {
            char buf[64];
            if (fgets(buf, sizeof(buf), pipe)) {
                int gpu_temp = std::atoi(buf);
                if (gpu_temp > 0 && gpu_temp < 150) {
                    ThermalZone zone;
                    zone.name = "gpu";
                    zone.type = "nvidia-smi";
                    zone.sysfs_path = "nvidia-smi";
                    zone.record_sample(static_cast<double>(gpu_temp));
                    zones_.push_back(std::move(zone));
                }
            }
            pclose(pipe);
        }
    }
}

VoidResult<std::string> ThermalModel::discover_zones() {
    zones_.clear();
    discover_thermal_zones();
    discover_hwmon_zones();
    discover_vpu_zone();
    discover_gpu_zone();

    if (zones_.empty()) {
        // Create a synthetic zone for systems where sysfs is not available
        // (e.g., containers, macOS dev builds).
        ThermalZone synthetic;
        synthetic.name = "synthetic_zone0";
        synthetic.type = "synthetic";
        synthetic.sysfs_path = "";
        synthetic.record_sample(45.0);
        TripPoint tp_passive{80, "passive"};
        TripPoint tp_critical{100, "critical"};
        synthetic.trip_points = {tp_passive, tp_critical};
        zones_.push_back(std::move(synthetic));
    }

    return VoidResult<std::string>::ok();
}

// ---------------------------------------------------------------------------
// Polling
// ---------------------------------------------------------------------------

VoidResult<std::string> ThermalModel::poll() {
    for (auto& zone : zones_) {
        double temp = 0.0;

        if (zone.type == "synthetic") {
            // Synthetic zone: hold steady with small random drift.
            double last = zone.history.empty() ? 45.0 : zone.history.back();
            // Tiny deterministic wobble based on history size.
            double wobble = (static_cast<int>(zone.history.size()) % 3 - 1) * 0.1;
            temp = last + wobble;
        } else if (zone.type == "nvidia-smi") {
            // Re-read from nvidia-smi.
            FILE* pipe = popen("nvidia-smi --query-gpu=temperature.gpu --format=csv,noheader,nounits 2>/dev/null", "r");
            if (pipe) {
                char buf[64];
                if (fgets(buf, sizeof(buf), pipe)) {
                    temp = static_cast<double>(std::atoi(buf));
                }
                pclose(pipe);
            }
        } else if (zone.type == "straylight-vpu") {
            // VPU: check for direct temp or pressure.
            namespace fs = std::filesystem;
            if (fs::exists("/sys/kernel/straylight-vpu/temperature")) {
                temp = read_sysfs_temp("/sys/kernel/straylight-vpu/temperature");
            } else if (fs::exists("/sys/kernel/straylight-vpu/memory_pressure")) {
                std::ifstream pf("/sys/kernel/straylight-vpu/memory_pressure");
                int pressure = 0;
                if (pf) pf >> pressure;
                temp = 40.0 + (pressure * 0.6);
            }
        } else {
            // Standard sysfs zone.
            temp = read_sysfs_temp(zone.sysfs_path.find("temp") != std::string::npos
                ? zone.sysfs_path
                : zone.sysfs_path + "/temp");
        }

        zone.record_sample(temp);
        zone.trend = compute_trend(zone);
    }

    return VoidResult<std::string>::ok();
}

// ---------------------------------------------------------------------------
// Prediction via linear regression
// ---------------------------------------------------------------------------

std::pair<double, double> ThermalModel::linear_regression(const std::deque<double>& samples) {
    size_t n = samples.size();
    if (n < 2) return {0.0, samples.empty() ? 0.0 : samples.back()};

    double sum_x = 0.0, sum_y = 0.0, sum_xy = 0.0, sum_x2 = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double x = static_cast<double>(i);
        double y = samples[i];
        sum_x  += x;
        sum_y  += y;
        sum_xy += x * y;
        sum_x2 += x * x;
    }

    double dn = static_cast<double>(n);
    double denom = dn * sum_x2 - sum_x * sum_x;
    if (std::abs(denom) < 1e-12) {
        return {0.0, sum_y / dn};
    }

    double slope = (dn * sum_xy - sum_x * sum_y) / denom;
    double intercept = (sum_y - slope * sum_x) / dn;
    return {slope, intercept};
}

double ThermalModel::predict_temperature(const ThermalZone& zone, int horizon_seconds) const {
    if (zone.history.size() < 2) {
        return static_cast<double>(zone.current_temp);
    }

    auto [slope, intercept] = linear_regression(zone.history);

    // Each sample is ~1 second apart (poll interval).
    // Predict at (last_index + horizon_seconds).
    double future_x = static_cast<double>(zone.history.size() - 1 + horizon_seconds);
    double predicted = slope * future_x + intercept;

    // Clamp to reasonable range.
    if (predicted < 0.0) predicted = 0.0;
    if (predicted > 150.0) predicted = 150.0;

    return predicted;
}

ThermalTrend ThermalModel::compute_trend(const ThermalZone& zone) {
    if (zone.history.size() < 3) return ThermalTrend::Stable;

    auto [slope, _] = linear_regression(zone.history);

    // Slope is in degrees-per-sample. If > 0.1 C/s => rising, < -0.1 => falling.
    constexpr double kThreshold = 0.1;
    if (slope > kThreshold) return ThermalTrend::Rising;
    if (slope < -kThreshold) return ThermalTrend::Falling;
    return ThermalTrend::Stable;
}

// ---------------------------------------------------------------------------
// Aggregated state
// ---------------------------------------------------------------------------

ThermalState ThermalModel::get_overall_thermal_state(const ThermalConfig& config) const {
    int max_temp = 0;
    for (const auto& z : zones_) {
        if (z.current_temp > max_temp) {
            max_temp = z.current_temp;
        }
    }

    if (max_temp >= config.critical_temp) return ThermalState::Critical;
    if (max_temp >= config.throttle_temp) return ThermalState::Hot;
    if (max_temp >= config.warn_temp)     return ThermalState::Warm;
    return ThermalState::Cool;
}

// ---------------------------------------------------------------------------
// Zone lookup
// ---------------------------------------------------------------------------

ThermalZone* ThermalModel::find_zone(const std::string& name) {
    for (auto& z : zones_) {
        if (z.name == name) return &z;
    }
    return nullptr;
}

const ThermalZone* ThermalModel::find_zone(const std::string& name) const {
    for (const auto& z : zones_) {
        if (z.name == name) return &z;
    }
    return nullptr;
}

} // namespace straylight::thermal
