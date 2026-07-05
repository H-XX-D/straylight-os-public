// services/watchdog/watchdog_engine.h
// Process watchdog engine — monitors critical services, auto-restarts on crash.
#pragma once

#include <straylight/result.h>
#include <straylight/error.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace straylight {

/// Health check method for a watched service.
enum class HealthCheckType {
    ProcStat,   // Check /proc/PID/stat for process existence
    HttpGet,    // HTTP GET to a health endpoint
    UnixSocket, // Connect to a Unix socket
    FileTouch,  // Check that a file was modified within a window
};

/// Escalation level when a service fails.
enum class EscalationLevel {
    Restart,  // Auto-restart via systemctl
    Notify,   // Send desktop notification + log
    Page,     // External page (webhook / email)
};

/// Configuration for a single watched service.
struct WatchedService {
    std::string name;
    std::string unit_name;   // systemd unit name
    HealthCheckType check_type = HealthCheckType::ProcStat;
    std::string check_target; // URL, socket path, or file path for health check
    int max_retries = 3;
    int backoff_base_seconds = 5;
    int health_check_interval_seconds = 30;
    std::vector<EscalationLevel> escalation_chain = {
        EscalationLevel::Restart,
        EscalationLevel::Notify,
        EscalationLevel::Page,
    };
};

/// Runtime state for a watched service.
struct ServiceState {
    std::string name;
    pid_t pid = 0;
    bool running = false;
    int consecutive_failures = 0;
    int total_restarts = 0;
    EscalationLevel current_escalation = EscalationLevel::Restart;
    std::chrono::steady_clock::time_point last_check;
    std::chrono::steady_clock::time_point last_restart;
    std::string last_error;
    std::vector<std::string> history;  // timestamped event log
};

/// Core watchdog engine.
class WatchdogEngine {
public:
    WatchdogEngine() = default;

    /// Add a service to watch.
    Result<void, SLError> watch(const WatchedService& svc);

    /// Remove a service from watch.
    Result<void, SLError> unwatch(const std::string& name);

    /// Check all watched services and take action on failures.
    Result<void, SLError> check_all();

    /// Check a single service.
    Result<bool, SLError> check_service(const std::string& name);

    /// Get the list of all watched service names.
    std::vector<std::string> list_services() const;

    /// Get state for a service.
    Result<ServiceState, SLError> get_state(const std::string& name) const;

    /// Get the full history for a service.
    Result<std::vector<std::string>, SLError> get_history(const std::string& name) const;

    /// Load watched services from config directory.
    Result<void, SLError> load_config(const std::filesystem::path& config_dir);

private:
    /// Check if a PID is alive via /proc/PID/stat.
    bool pid_alive(pid_t pid) const;

    /// Resolve the PID of a systemd unit.
    pid_t resolve_pid(const std::string& unit_name) const;

    /// Perform a health check based on type.
    bool perform_health_check(const WatchedService& svc);

    /// Restart a service via systemctl.
    Result<void, SLError> restart_service(const std::string& unit_name);

    /// Send a notification (desktop/log).
    void send_notification(const std::string& service_name, const std::string& message);

    /// Send a page (webhook).
    void send_page(const std::string& service_name, const std::string& message);

    /// Add a timestamped history entry.
    void add_history(ServiceState& state, const std::string& event);

    /// Escalate failure handling.
    void escalate(const WatchedService& svc, ServiceState& state);

    mutable std::mutex mutex_;
    std::map<std::string, WatchedService> services_;
    std::map<std::string, ServiceState> states_;
};

} // namespace straylight
