/**
 * StrayLight Thermal Logger — Implementation.
 */

#include "thermal_log.h"

#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace straylight::thermal {

ThermalLog::ThermalLog(const std::string& log_path)
    : log_path_(log_path)
    , last_budget_update_(std::chrono::steady_clock::now())
{}

ThermalLog::~ThermalLog() {
    if (log_file_.is_open()) {
        log_file_.flush();
        log_file_.close();
    }
}

VoidResult<std::string> ThermalLog::init() {
    namespace fs = std::filesystem;

    auto parent = fs::path(log_path_).parent_path();
    std::error_code ec;
    fs::create_directories(parent, ec);
    if (ec) {
        // Non-fatal: we may not have permissions in dev environments.
        fprintf(stderr, "[thermal-log] warning: cannot create log directory %s: %s\n",
                parent.c_str(), ec.message().c_str());
    }

    log_file_.open(log_path_, std::ios::app);
    if (!log_file_.is_open()) {
        // Try a fallback path.
        log_path_ = "/tmp/straylight-thermal.log";
        log_file_.open(log_path_, std::ios::app);
        if (!log_file_.is_open()) {
            return VoidResult<std::string>::error("cannot open log file: " + log_path_);
        }
        fprintf(stderr, "[thermal-log] using fallback log path: %s\n", log_path_.c_str());
    }

    write_line("=== straylight-thermal log started ===");
    return VoidResult<std::string>::ok();
}

std::string ThermalLog::format_timestamp(std::chrono::system_clock::time_point tp) {
    auto time_t = std::chrono::system_clock::to_time_t(tp);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        tp.time_since_epoch()) % 1000;

    std::tm tm_buf{};
    localtime_r(&time_t, &tm_buf);

    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

void ThermalLog::write_line(const std::string& line) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (log_file_.is_open()) {
        auto now = std::chrono::system_clock::now();
        log_file_ << format_timestamp(now) << " " << line << "\n";
    }
}

void ThermalLog::log_poll(const ThermalModel& model,
                           const ThermalConfig& config,
                           const ThrottleController& throttle)
{
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - last_budget_update_).count();

    if (elapsed > 0) {
        last_budget_update_ = now;
        budget_.total_seconds += elapsed;

        ThermalState current_state = model.get_overall_thermal_state(config);
        switch (current_state) {
            case ThermalState::Cool:     budget_.cool_seconds += elapsed; break;
            case ThermalState::Warm:     budget_.warm_seconds += elapsed; break;
            case ThermalState::Hot:      budget_.hot_seconds += elapsed; break;
            case ThermalState::Critical: budget_.critical_seconds += elapsed; break;
        }

        // Log state transitions.
        if (current_state != last_state_) {
            std::ostringstream oss;
            oss << "STATE_CHANGE " << state_to_string(last_state_)
                << " -> " << state_to_string(current_state);
            write_line(oss.str());

            ThermalEvent evt;
            evt.timestamp = std::chrono::system_clock::now();
            evt.zone_name = "system";
            evt.temperature = 0;
            evt.trend = ThermalTrend::Stable;
            evt.state = current_state;
            evt.action = "state_change";

            // Find max temp zone.
            for (const auto& z : model.zones()) {
                if (z.current_temp > evt.temperature) {
                    evt.temperature = z.current_temp;
                    evt.zone_name = z.name;
                    evt.trend = z.trend;
                }
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                events_.push_back(evt);
            }

            last_state_ = current_state;
        }
    }

    // Periodic zone temperature log (every poll).
    std::ostringstream oss;
    oss << "POLL";
    for (const auto& zone : model.zones()) {
        oss << " " << zone.name << "=" << zone.current_temp << "C"
            << "(" << trend_to_string(zone.trend) << ")";
    }
    oss << " state=" << state_to_string(model.get_overall_thermal_state(config));
    if (throttle.is_throttled()) {
        oss << " throttled=" << throttle.active_count();
    }
    write_line(oss.str());
}

void ThermalLog::log_event(const ThermalEvent& event) {
    std::ostringstream oss;
    oss << "EVENT zone=" << event.zone_name
        << " temp=" << event.temperature << "C"
        << " trend=" << trend_to_string(event.trend)
        << " state=" << state_to_string(event.state)
        << " action=" << event.action;
    write_line(oss.str());

    std::lock_guard<std::mutex> lock(mutex_);
    events_.push_back(event);
}

void ThermalLog::log_throttle_change(const std::string& action, int temp, double predicted) {
    std::ostringstream oss;
    oss << "THROTTLE " << action
        << " temp=" << temp << "C"
        << " predicted=" << std::fixed << std::setprecision(1) << predicted << "C";
    write_line(oss.str());

    ThermalEvent evt;
    evt.timestamp = std::chrono::system_clock::now();
    evt.zone_name = "system";
    evt.temperature = temp;
    evt.trend = ThermalTrend::Rising;
    evt.state = ThermalState::Hot;
    evt.action = action;

    std::lock_guard<std::mutex> lock(mutex_);
    events_.push_back(evt);
}

std::vector<ThermalEvent> ThermalLog::recent_events(size_t count) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (events_.size() <= count) {
        return events_;
    }
    return {events_.end() - static_cast<ptrdiff_t>(count), events_.end()};
}

void ThermalLog::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (log_file_.is_open()) {
        log_file_.flush();
    }
}

} // namespace straylight::thermal
