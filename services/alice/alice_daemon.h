// services/alice/alice_daemon.h
#pragma once

#include <straylight/daemon.h>
#include "alice_engine.h"
#include "alert_manager.h"
#include "ipc_server.h"
#include "log_analyzer.h"

namespace straylight {

/// Alice daemon — AI-powered system health monitor for StrayLight OS.
/// Periodically collects system telemetry, runs inference with a local GGUF model,
/// and emits alerts through D-Bus / socket / log file channels.
class AliceDaemon : public DaemonBase {
public:
    Result<void, SLError> init(const Config& cfg) override;
    Result<void, SLError> tick() override;
    void shutdown() override;

private:
    AliceEngine engine_;
    LogAnalyzer analyzer_;
    AlertManager alerts_;
    AliceIpcServer ipc_;

    int tick_interval_s_ = 30;
    int log_window_s_ = 300;
    int max_log_entries_ = 100;

    /// Run a full monitoring cycle: collect data, analyze, emit alerts.
    void monitor_cycle();

    /// Check a subsystem and emit alerts if thresholds exceeded.
    void check_thermal();
    void check_memory();
    void check_gpu();
    void check_disk();
    void check_network();
    void check_logs();
};

} // namespace straylight
