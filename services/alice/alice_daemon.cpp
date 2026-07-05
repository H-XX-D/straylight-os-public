// services/alice/alice_daemon.cpp
#include "alice_daemon.h"
#include <straylight/log.h>

#include <chrono>
#include <sstream>
#include <thread>

namespace straylight {

Result<void, SLError> AliceDaemon::init(const Config& cfg) {
    SL_INFO("alice: initializing daemon");

    // -----------------------------------------------------------------------
    // Model config
    // -----------------------------------------------------------------------
    AliceEngine::ModelConfig mcfg;
    mcfg.model_path = cfg.get<std::string>(
        "model.path",
        "");
    mcfg.context_size = cfg.get<int>("model.context_size", 2048);
    mcfg.threads = cfg.get<int>("model.threads", 4);
    mcfg.temperature = cfg.get<float>("model.temperature", 0.3f);
    mcfg.gpu_offload = cfg.get<bool>("model.gpu_offload", true);
    mcfg.gpu_layers = cfg.get<int>("model.gpu_layers", -1);
    mcfg.idle_unload_seconds = cfg.get<int>("model.idle_unload_seconds", 60);

    engine_.configure(mcfg);
    engine_.start_idle_timer();

    // -----------------------------------------------------------------------
    // Monitor config
    // -----------------------------------------------------------------------
    tick_interval_s_ = cfg.get<int>("monitor.tick_interval_seconds", 30);
    log_window_s_ = cfg.get<int>("monitor.log_window_seconds", 300);
    max_log_entries_ = cfg.get<int>("monitor.max_log_entries", 100);

    // -----------------------------------------------------------------------
    // Alert config
    // -----------------------------------------------------------------------
    std::vector<AlertChannel> channels;
    if (cfg.has("alerts.channels")) {
        // Config stores channels as a JSON array of strings
        auto raw = cfg.raw();
        if (raw.contains("alerts") && raw["alerts"].contains("channels") &&
            raw["alerts"]["channels"].is_array()) {
            for (const auto& ch : raw["alerts"]["channels"]) {
                if (ch.is_string()) {
                    channels.push_back(AlertManager::channel_from_string(ch.get<std::string>()));
                }
            }
        }
    }
    if (channels.empty()) {
        channels = {AlertChannel::Console, AlertChannel::File};
    }

    std::string log_path = cfg.get<std::string>(
        "alerts.log_path", "/var/log/straylight/alice.log");
    std::string socket_path = cfg.get<std::string>(
        "ipc.socket_path", "/run/straylight/alice.sock");

    alerts_.configure(channels, log_path, socket_path);

    // Set per-category thresholds
    if (cfg.has("alerts.thresholds")) {
        auto raw = cfg.raw();
        if (raw.contains("alerts") && raw["alerts"].contains("thresholds") &&
            raw["alerts"]["thresholds"].is_object()) {
            for (auto& [key, val] : raw["alerts"]["thresholds"].items()) {
                if (val.is_string()) {
                    alerts_.set_threshold(key,
                        AlertManager::severity_from_string(val.get<std::string>()));
                }
            }
        }
    }

    // -----------------------------------------------------------------------
    // IPC server
    // -----------------------------------------------------------------------
    ipc_.set_engine(&engine_);
    ipc_.set_analyzer(&analyzer_);
    ipc_.set_alerts(&alerts_);

    int max_clients = cfg.get<int>("ipc.max_clients", 4);
    auto ipc_result = ipc_.start(socket_path, max_clients);
    if (!ipc_result.has_value()) {
        SL_WARN("alice: IPC server failed to start: {}", ipc_result.error());
        // Non-fatal: daemon can still run without IPC
    }

    SL_INFO("alice: daemon initialized (tick={}s, log_window={}s)",
            tick_interval_s_, log_window_s_);
    return Result<void, SLError>::ok();
}

Result<void, SLError> AliceDaemon::tick() {
    monitor_cycle();
    std::this_thread::sleep_for(std::chrono::seconds(tick_interval_s_));
    return Result<void, SLError>::ok();
}

void AliceDaemon::shutdown() {
    SL_INFO("alice: shutting down");
    ipc_.stop();
    engine_.stop_idle_timer();
    engine_.unload();
    SL_INFO("alice: shutdown complete");
}

// ---------------------------------------------------------------------------
// Monitoring cycle
// ---------------------------------------------------------------------------

void AliceDaemon::monitor_cycle() {
    SL_DEBUG("alice: starting monitor cycle");

    check_thermal();
    check_memory();
    check_gpu();
    check_disk();
    check_network();
    check_logs();

    SL_DEBUG("alice: monitor cycle complete");
}

void AliceDaemon::check_thermal() {
    auto result = analyzer_.thermal_status();
    if (!result.has_value()) return;

    const auto& report = result.value();

    // Simple threshold check: look for temperatures above warning levels
    // Parse "NNC" temperature values from the report
    std::istringstream stream(report);
    std::string line;
    while (std::getline(stream, line)) {
        auto pos = line.find("C");
        if (pos == std::string::npos || pos < 2) continue;

        // Find the number before "C"
        auto num_end = pos;
        auto num_start = line.find_last_not_of("0123456789", num_end - 1);
        if (num_start == std::string::npos) num_start = 0;
        else num_start += 1;

        if (num_start >= num_end) continue;

        try {
            int temp = std::stoi(line.substr(num_start, num_end - num_start));
            if (temp >= 95) {
                Alert a;
                a.severity = AlertSeverity::Critical;
                a.category = "thermal";
                a.title = "Critical temperature";
                a.detail = line;
                alerts_.emit(std::move(a));
            } else if (temp >= 80) {
                Alert a;
                a.severity = AlertSeverity::Warning;
                a.category = "thermal";
                a.title = "High temperature";
                a.detail = line;
                alerts_.emit(std::move(a));
            }
        } catch (...) {}
    }
}

void AliceDaemon::check_memory() {
    auto result = analyzer_.memory_pressure();
    if (!result.has_value()) return;

    const auto& report = result.value();

    // Look for "Usage: NN%" line
    auto pos = report.find("Usage:");
    if (pos != std::string::npos) {
        auto num_start = report.find_first_of("0123456789", pos);
        if (num_start != std::string::npos) {
            auto num_end = report.find_first_not_of("0123456789", num_start);
            try {
                int usage = std::stoi(report.substr(num_start, num_end - num_start));
                if (usage >= 95) {
                    Alert a;
                    a.severity = AlertSeverity::Critical;
                    a.category = "memory";
                    a.title = "Memory critically low";
                    a.detail = "Memory usage at " + std::to_string(usage) + "%";
                    alerts_.emit(std::move(a));
                } else if (usage >= 85) {
                    Alert a;
                    a.severity = AlertSeverity::Warning;
                    a.category = "memory";
                    a.title = "Memory pressure high";
                    a.detail = "Memory usage at " + std::to_string(usage) + "%";
                    alerts_.emit(std::move(a));
                }
            } catch (...) {}
        }
    }

    // Check for OOM in report
    if (report.find("OOM") != std::string::npos || report.find("oom") != std::string::npos) {
        Alert a;
        a.severity = AlertSeverity::Critical;
        a.category = "memory";
        a.title = "OOM condition detected";
        a.detail = "Out-of-memory killer activity detected";
        alerts_.emit(std::move(a));
    }
}

void AliceDaemon::check_gpu() {
    auto result = analyzer_.gpu_health();
    if (!result.has_value()) return;

    const auto& report = result.value();

    // Check for GPU temperature in report
    auto pos = report.find("Temperature:");
    while (pos != std::string::npos) {
        auto num_start = report.find_first_of("0123456789", pos);
        if (num_start != std::string::npos) {
            auto num_end = report.find_first_not_of("0123456789", num_start);
            try {
                int temp = std::stoi(report.substr(num_start, num_end - num_start));
                if (temp >= 95) {
                    Alert a;
                    a.severity = AlertSeverity::Critical;
                    a.category = "gpu";
                    a.title = "GPU temperature critical";
                    a.detail = "GPU at " + std::to_string(temp) + "C";
                    alerts_.emit(std::move(a));
                } else if (temp >= 80) {
                    Alert a;
                    a.severity = AlertSeverity::Warning;
                    a.category = "gpu";
                    a.title = "GPU temperature elevated";
                    a.detail = "GPU at " + std::to_string(temp) + "C";
                    alerts_.emit(std::move(a));
                }
            } catch (...) {}
        }
        pos = report.find("Temperature:", pos + 12);
    }

    // Check for Xid errors (NVIDIA GPU faults)
    if (report.find("Xid") != std::string::npos) {
        Alert a;
        a.severity = AlertSeverity::Error;
        a.category = "gpu";
        a.title = "NVIDIA Xid error";
        a.detail = "GPU Xid error detected — possible hardware fault";
        alerts_.emit(std::move(a));
    }
}

void AliceDaemon::check_disk() {
    auto result = analyzer_.disk_health();
    if (!result.has_value()) return;

    const auto& report = result.value();

    // Check for SMART failures
    if (report.find("FAILING") != std::string::npos) {
        Alert a;
        a.severity = AlertSeverity::Critical;
        a.category = "disk";
        a.title = "Disk SMART failure";
        a.detail = "SMART health check reports FAILING — drive may be dying";
        alerts_.emit(std::move(a));
    }
}

void AliceDaemon::check_network() {
    auto result = analyzer_.network_status();
    if (!result.has_value()) return;

    const auto& report = result.value();

    // Check for link down
    if (report.find("down") != std::string::npos) {
        // Find which interface
        std::istringstream stream(report);
        std::string line;
        std::string current_iface;
        while (std::getline(stream, line)) {
            // Interface lines end with ':'
            auto colon = line.find(':');
            if (colon != std::string::npos && line.find("  ") != 0) {
                current_iface = line.substr(0, colon);
            }
            if (line.find("State: down") != std::string::npos) {
                Alert a;
                a.severity = AlertSeverity::Warning;
                a.category = "network";
                a.title = "Interface down";
                a.detail = current_iface + " is in down state";
                alerts_.emit(std::move(a));
            }
        }
    }

    // Check for packet errors
    if (report.find("ERRORS") != std::string::npos) {
        Alert a;
        a.severity = AlertSeverity::Warning;
        a.category = "network";
        a.title = "Network errors detected";
        a.detail = "Packet errors on one or more interfaces";
        alerts_.emit(std::move(a));
    }
}

void AliceDaemon::check_logs() {
    auto errors = analyzer_.collect_errors(std::chrono::seconds(log_window_s_));
    if (!errors.has_value()) return;

    const auto& entries = errors.value();
    if (entries.empty()) return;

    // Count critical vs error
    int crit_count = 0;
    int err_count = 0;
    for (const auto& e : entries) {
        if (e.level == "crit") ++crit_count;
        else if (e.level == "err") ++err_count;
    }

    if (crit_count > 0) {
        // Get AI analysis for critical entries
        std::string ai_analysis;
        auto summary = analyzer_.summarize_for_ai(entries);
        if (summary.has_value()) {
            auto ai = engine_.analyze(summary.value());
            if (ai.has_value()) {
                ai_analysis = ai.value();
            }
        }

        Alert a;
        a.severity = AlertSeverity::Critical;
        a.category = "system";
        a.title = "Critical log entries detected";
        a.detail = std::to_string(crit_count) + " critical entries in last " +
                   std::to_string(log_window_s_) + "s";
        a.ai_analysis = ai_analysis;
        alerts_.emit(std::move(a));
    } else if (err_count >= 10) {
        Alert a;
        a.severity = AlertSeverity::Warning;
        a.category = "system";
        a.title = "Elevated error rate in logs";
        a.detail = std::to_string(err_count) + " errors in last " +
                   std::to_string(log_window_s_) + "s";
        alerts_.emit(std::move(a));
    }
}

} // namespace straylight
