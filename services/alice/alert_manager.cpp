// services/alice/alert_manager.cpp
#include "alert_manager.h"
#include <straylight/log.h>

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace straylight {

AlertManager::AlertManager() = default;

void AlertManager::configure(const std::vector<AlertChannel>& channels,
                              const std::string& log_path,
                              const std::string& socket_path) {
    std::lock_guard lock(mutex_);
    channels_ = channels;
    log_path_ = log_path;
    socket_path_ = socket_path;

    // Ensure log directory exists
    if (!log_path_.empty()) {
        std::error_code ec;
        auto parent = std::filesystem::path(log_path_).parent_path();
        std::filesystem::create_directories(parent, ec);
        // Ignore errors — may not have permission, that's fine
    }
}

void AlertManager::set_threshold(const std::string& category, AlertSeverity min_severity) {
    std::lock_guard lock(mutex_);
    thresholds_[category] = min_severity;
}

void AlertManager::emit(Alert alert) {
    alert.timestamp = std::chrono::system_clock::now();

    std::lock_guard lock(mutex_);

    if (!passes_threshold(alert)) return;

    // Add to history ring buffer
    history_.push_back(alert);
    if (history_.size() > MAX_HISTORY) {
        history_.pop_front();
    }

    // Route to configured channels
    for (auto channel : channels_) {
        switch (channel) {
            case AlertChannel::Console: emit_console(alert); break;
            case AlertChannel::File:    emit_file(alert); break;
            case AlertChannel::Socket:  emit_socket(alert); break;
            case AlertChannel::DBus:    emit_dbus(alert); break;
        }
    }
}

std::vector<Alert> AlertManager::recent(int count) const {
    std::lock_guard lock(mutex_);

    int n = std::min(count, static_cast<int>(history_.size()));
    std::vector<Alert> result;
    result.reserve(static_cast<size_t>(n));

    // Return most recent first
    auto it = history_.rbegin();
    for (int i = 0; i < n && it != history_.rend(); ++i, ++it) {
        result.push_back(*it);
    }

    return result;
}

std::string AlertManager::severity_to_string(AlertSeverity sev) {
    switch (sev) {
        case AlertSeverity::Info:     return "info";
        case AlertSeverity::Warning:  return "warning";
        case AlertSeverity::Error:    return "error";
        case AlertSeverity::Critical: return "critical";
    }
    return "unknown";
}

AlertSeverity AlertManager::severity_from_string(const std::string& s) {
    if (s == "info")     return AlertSeverity::Info;
    if (s == "warning")  return AlertSeverity::Warning;
    if (s == "error")    return AlertSeverity::Error;
    if (s == "critical") return AlertSeverity::Critical;
    return AlertSeverity::Info;
}

AlertChannel AlertManager::channel_from_string(const std::string& s) {
    if (s == "console") return AlertChannel::Console;
    if (s == "file")    return AlertChannel::File;
    if (s == "socket")  return AlertChannel::Socket;
    if (s == "dbus")    return AlertChannel::DBus;
    return AlertChannel::Console;
}

// ---------------------------------------------------------------------------
// Channel implementations
// ---------------------------------------------------------------------------

void AlertManager::emit_console(const Alert& alert) {
    std::string sev = severity_to_string(alert.severity);

    switch (alert.severity) {
        case AlertSeverity::Critical:
            SL_CRITICAL("alice [{}] {}: {}", alert.category, alert.title, alert.detail);
            break;
        case AlertSeverity::Error:
            SL_ERROR("alice [{}] {}: {}", alert.category, alert.title, alert.detail);
            break;
        case AlertSeverity::Warning:
            SL_WARN("alice [{}] {}: {}", alert.category, alert.title, alert.detail);
            break;
        case AlertSeverity::Info:
            SL_INFO("alice [{}] {}: {}", alert.category, alert.title, alert.detail);
            break;
    }
}

void AlertManager::emit_file(const Alert& alert) {
    if (log_path_.empty()) return;

    std::ofstream ofs(log_path_, std::ios::app);
    if (!ofs.is_open()) return;

    auto time_t_val = std::chrono::system_clock::to_time_t(alert.timestamp);
    std::tm tm_val{};
    ::localtime_r(&time_t_val, &tm_val);

    ofs << std::put_time(&tm_val, "%Y-%m-%dT%H:%M:%S")
        << " [" << severity_to_string(alert.severity) << "]"
        << " [" << alert.category << "]"
        << " " << alert.title
        << ": " << alert.detail;

    if (!alert.ai_analysis.empty()) {
        ofs << " | AI: " << alert.ai_analysis;
    }
    ofs << "\n";
    ofs.flush();
}

void AlertManager::emit_socket(const Alert& alert) {
    // Socket emission is handled by the IPC server broadcasting to connected clients.
    // This method is a no-op — the IPC server queries recent() to serve alerts.
    (void)alert;
}

void AlertManager::emit_dbus(const Alert& alert) {
    // D-Bus signal emission requires sdbus-c++ which is Linux-only.
    // On macOS builds this is a no-op. On Linux, the Alice daemon would
    // call sd_bus_emit_signal on org.straylight.Alice1 /org/straylight/Alice1
    // with interface org.straylight.Alice1.Monitor, signal name "Alert".
    (void)alert;
    SL_DEBUG("alice: would emit D-Bus Alert signal for [{}] {}",
             alert.category, alert.title);
}

bool AlertManager::passes_threshold(const Alert& alert) const {
    auto it = thresholds_.find(alert.category);
    if (it == thresholds_.end()) {
        // No threshold set — allow everything
        return true;
    }

    // Compare severity levels (higher enum value = more severe)
    return static_cast<int>(alert.severity) >= static_cast<int>(it->second);
}

} // namespace straylight
