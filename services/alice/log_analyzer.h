// services/alice/log_analyzer.h
#pragma once

#include <straylight/result.h>

#include <chrono>
#include <string>
#include <vector>

namespace straylight {

/// Collects and analyzes system logs from kernel, journald, and sysfs.
class LogAnalyzer {
public:
    struct LogEntry {
        std::chrono::system_clock::time_point timestamp;
        std::string source;  // "dmesg", "journalctl", "syslog"
        std::string level;   // "info", "warn", "err", "crit"
        std::string message;
    };

    LogAnalyzer() = default;

    /// Collect recent log entries from all sources.
    Result<std::vector<LogEntry>, std::string> collect_recent(int max_entries = 100);

    /// Collect only error/critical entries within the given time window.
    Result<std::vector<LogEntry>, std::string> collect_errors(
        std::chrono::seconds window = std::chrono::seconds(300));

    /// Format log entries into a structured text block for AI prompt consumption.
    Result<std::string, std::string> summarize_for_ai(const std::vector<LogEntry>& entries);

    /// GPU health: temperature, utilization, VRAM usage from sysfs and nvidia-smi.
    Result<std::string, std::string> gpu_health();

    /// Thermal status from /sys/class/thermal and /sys/class/hwmon.
    Result<std::string, std::string> thermal_status();

    /// Memory pressure from /proc/meminfo and /proc/pressure/memory.
    Result<std::string, std::string> memory_pressure();

    /// Disk health from SMART data and /proc/diskstats.
    Result<std::string, std::string> disk_health();

    /// Network status from /sys/class/net and /proc/net/dev.
    Result<std::string, std::string> network_status();

private:
    /// Read a sysfs file, returning empty string on failure.
    static std::string read_sysfs(const std::string& path);

    /// Run a command and capture stdout, returning empty string on failure.
    static std::string run_command(const std::string& cmd);

    /// Parse a dmesg-style priority/facility prefix (e.g., "<3>").
    static std::string priority_to_level(int priority);

    /// Parse kmsg entries from /dev/kmsg.
    std::vector<LogEntry> parse_kmsg(int max_entries);

    /// Parse journalctl JSON output.
    std::vector<LogEntry> parse_journalctl(int max_entries,
                                            const std::string& since = "");
};

} // namespace straylight
