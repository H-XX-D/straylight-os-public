// tools/log/log_engine.h
// Unified log viewer and search engine for StrayLight OS.
#pragma once

#include <straylight/result.h>

#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace straylight {

/// Represents a single log entry from any source.
struct LogEntry {
    std::chrono::system_clock::time_point timestamp;
    std::string service;
    std::string hostname;
    pid_t pid = 0;

    enum class Level { Debug, Info, Warning, Error, Critical };
    Level level = Level::Info;

    std::string message;
    std::string source;   // "journald", "syslog", "straylight"
    std::string unit;     // systemd unit name if applicable
};

/// Log statistics for a service.
struct LogStats {
    std::string service;
    uint64_t total_entries = 0;
    uint64_t debug_count = 0;
    uint64_t info_count = 0;
    uint64_t warn_count = 0;
    uint64_t error_count = 0;
    uint64_t critical_count = 0;
    uint64_t bytes_total = 0;
    std::chrono::system_clock::time_point first_entry;
    std::chrono::system_clock::time_point last_entry;
};

/// Alert rule configuration.
struct AlertRule {
    uint32_t id = 0;
    std::string pattern;       // regex pattern to match
    std::string service;       // limit to specific service (empty = all)
    LogEntry::Level min_level = LogEntry::Level::Error;
    std::string action;        // "notify", "exec:<cmd>", "log:<file>"
    bool enabled = true;
    uint32_t trigger_count = 0; // how many times this alert has fired
};

class LogEngine {
public:
    LogEngine();
    ~LogEngine();

    /// Query logs with filters.
    Result<std::vector<LogEntry>, std::string> query(
        const std::string& service = "",
        const std::string& since = "",
        const std::string& until = "",
        LogEntry::Level min_level = LogEntry::Level::Debug,
        int max_entries = 1000) const;

    /// Search logs using a regex pattern.
    Result<std::vector<LogEntry>, std::string> search(
        const std::string& pattern,
        const std::string& service = "",
        const std::string& since = "",
        int max_entries = 500) const;

    /// Follow logs in real-time (calls callback for each new entry).
    /// Returns when callback returns false or on error.
    Result<void, std::string> follow(
        const std::string& service,
        std::function<bool(const LogEntry&)> callback) const;

    /// Get log statistics per service.
    Result<std::vector<LogStats>, std::string> stats(
        const std::string& since = "") const;

    /// Export logs to a specific format.
    Result<std::string, std::string> export_logs(
        const std::string& format,  // "json", "csv", "text"
        const std::string& service = "",
        const std::string& since = "",
        const std::string& until = "",
        int max_entries = 10000) const;

    /// Add an alert rule.
    Result<void, std::string> add_alert(const AlertRule& rule);

    /// Remove an alert rule.
    Result<void, std::string> remove_alert(uint32_t rule_id);

    /// List alert rules.
    std::vector<AlertRule> list_alerts() const;

    /// Check a log entry against alert rules and fire matching ones.
    void check_alerts(const LogEntry& entry);

private:
    /// Read from journalctl with given arguments.
    Result<std::vector<LogEntry>, std::string> read_journald(
        const std::string& args) const;

    /// Read from /var/log/syslog.
    Result<std::vector<LogEntry>, std::string> read_syslog(
        const std::string& since = "",
        const std::string& until = "") const;

    /// Read from /var/log/straylight/*.log.
    Result<std::vector<LogEntry>, std::string> read_straylight_logs(
        const std::string& service = "") const;

    /// Parse a journalctl JSON line into a LogEntry.
    LogEntry parse_journald_json(const std::string& json) const;

    /// Parse a syslog line into a LogEntry.
    LogEntry parse_syslog_line(const std::string& line) const;

    /// Parse a straylight log line.
    LogEntry parse_straylight_line(const std::string& line,
                                    const std::string& source) const;

    /// Run a shell command and capture output.
    Result<std::string, std::string> run_cmd(const std::string& cmd) const;

    /// Parse a log level string.
    static LogEntry::Level parse_level(const std::string& level_str);

    /// Convert a level to string.
    static std::string level_to_string(LogEntry::Level level);

    /// Fire an alert action.
    void fire_alert(const AlertRule& rule, const LogEntry& entry) const;

    /// Read/write alerts config.
    std::string alerts_config_path() const;
    void load_alerts();
    void save_alerts() const;

    std::vector<AlertRule> alerts_;
    uint32_t next_alert_id_ = 1;
};

} // namespace straylight
