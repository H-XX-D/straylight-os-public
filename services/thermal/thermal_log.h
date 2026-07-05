/**
 * StrayLight Thermal Logger — Logs thermal events and tracks thermal budget.
 *
 * Writes structured log entries to /var/log/straylight/thermal.log with
 * timestamps, zone temperatures, throttle actions, and thermal state.
 * Tracks cumulative thermal budget (time spent above warn/throttle thresholds).
 */
#pragma once

#include "thermal_model.h"
#include "throttle_controller.h"
#include "straylight/result.h"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace straylight::thermal {

/// A recorded thermal event.
struct ThermalEvent {
    std::chrono::system_clock::time_point timestamp;
    std::string zone_name;
    int temperature;
    ThermalTrend trend;
    ThermalState state;
    std::string action;  // e.g., "throttle_engaged", "throttle_released", "critical_warning"
};

/// Thermal budget tracking — how long the system has spent in each state.
struct ThermalBudget {
    uint64_t cool_seconds = 0;
    uint64_t warm_seconds = 0;
    uint64_t hot_seconds = 0;
    uint64_t critical_seconds = 0;
    uint64_t total_seconds = 0;

    double hot_percentage() const {
        if (total_seconds == 0) return 0.0;
        return (static_cast<double>(hot_seconds + critical_seconds) /
                static_cast<double>(total_seconds)) * 100.0;
    }
};

class ThermalLog {
public:
    explicit ThermalLog(const std::string& log_path = "/var/log/straylight/thermal.log");
    ~ThermalLog();

    /// Initialize the log file (create directories, open file).
    VoidResult<std::string> init();

    /// Log current thermal state for all zones.
    void log_poll(const ThermalModel& model,
                  const ThermalConfig& config,
                  const ThrottleController& throttle);

    /// Log a specific thermal event.
    void log_event(const ThermalEvent& event);

    /// Log a throttle state change.
    void log_throttle_change(const std::string& action, int temp, double predicted);

    /// Update and return the thermal budget.
    const ThermalBudget& budget() const { return budget_; }

    /// Get recent events (last N).
    std::vector<ThermalEvent> recent_events(size_t count) const;

    /// Get the full event history.
    const std::vector<ThermalEvent>& events() const { return events_; }

    /// Flush log file.
    void flush();

private:
    std::string log_path_;
    std::ofstream log_file_;
    std::vector<ThermalEvent> events_;
    ThermalBudget budget_;
    ThermalState last_state_ = ThermalState::Cool;
    std::chrono::steady_clock::time_point last_budget_update_;
    mutable std::mutex mutex_;

    /// Format a timestamp for log output.
    static std::string format_timestamp(std::chrono::system_clock::time_point tp);

    /// Write a line to the log file.
    void write_line(const std::string& line);
};

} // namespace straylight::thermal
