// apps/system_monitor/network.h
// Network monitoring via /proc/net/dev
#pragma once

#include <straylight/result.h>

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace straylight::sysmon {

/// Per-interface network stats.
struct InterfaceStats {
    std::string name;
    uint64_t rx_bytes = 0;
    uint64_t tx_bytes = 0;
    uint64_t rx_packets = 0;
    uint64_t tx_packets = 0;
    uint64_t rx_errors = 0;
    uint64_t tx_errors = 0;
    uint64_t rx_dropped = 0;
    uint64_t tx_dropped = 0;

    // Calculated bandwidth (bytes/sec)
    double rx_bps = 0.0;
    double tx_bps = 0.0;

    std::deque<float> rx_history; // KB/s
    std::deque<float> tx_history; // KB/s

    static constexpr int kMaxHistory = 60;
};

/// Network monitor.
class NetworkMonitor {
public:
    NetworkMonitor();

    /// Sample current network stats.
    Result<void, std::string> sample();

    /// Get per-interface stats.
    [[nodiscard]] const std::vector<InterfaceStats>& interfaces() const {
        return interfaces_;
    }

    /// Render network tab in ImGui.
    void render();

private:
    std::vector<InterfaceStats> interfaces_;
    std::vector<InterfaceStats> prev_interfaces_;
    bool first_sample_ = true;
    double last_sample_time_ = 0.0;

    double get_time_seconds() const;
};

/// Format bytes per second to human-readable string.
std::string format_bandwidth(double bps);

/// Format total bytes to human-readable string.
std::string format_bytes(uint64_t bytes);

} // namespace straylight::sysmon
