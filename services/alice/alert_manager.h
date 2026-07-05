// services/alice/alert_manager.h
#pragma once

#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace straylight {

enum class AlertSeverity { Info, Warning, Error, Critical };
enum class AlertChannel { Console, File, Socket, DBus };

struct Alert {
    AlertSeverity severity;
    std::string category;  // "gpu", "thermal", "memory", "disk", "network", "system"
    std::string title;
    std::string detail;
    std::string ai_analysis;  // Alice's assessment
    std::chrono::system_clock::time_point timestamp;
};

/// Manages alert emission, history, and routing to configured channels.
class AlertManager {
public:
    AlertManager();

    /// Configure which channels receive alerts.
    void configure(const std::vector<AlertChannel>& channels,
                   const std::string& log_path = "/var/log/straylight/alice.log",
                   const std::string& socket_path = "");

    /// Set the minimum severity threshold for a category.
    void set_threshold(const std::string& category, AlertSeverity min_severity);

    /// Emit an alert to all configured channels (respecting thresholds).
    void emit(Alert alert);

    /// Retrieve the most recent alerts from history.
    std::vector<Alert> recent(int count = 50) const;

    /// Convert AlertSeverity to string.
    static std::string severity_to_string(AlertSeverity sev);

    /// Parse a severity string.
    static AlertSeverity severity_from_string(const std::string& s);

    /// Convert AlertChannel from string.
    static AlertChannel channel_from_string(const std::string& s);

private:
    static constexpr size_t MAX_HISTORY = 1000;

    mutable std::mutex mutex_;
    std::deque<Alert> history_;
    std::vector<AlertChannel> channels_;
    std::unordered_map<std::string, AlertSeverity> thresholds_;
    std::string log_path_;
    std::string socket_path_;

    void emit_console(const Alert& alert);
    void emit_file(const Alert& alert);
    void emit_socket(const Alert& alert);
    void emit_dbus(const Alert& alert);

    bool passes_threshold(const Alert& alert) const;
};

} // namespace straylight
