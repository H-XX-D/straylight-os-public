// apps/hub/dashboard.h
// System health dashboard — CPU/RAM/GPU gauges, uptime, disk, network, alerts.
#pragma once

#include <imgui.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace straylight::hub {

struct SystemHealth {
    float cpu_usage = 0.0f;
    float mem_usage = 0.0f;
    float gpu_usage = 0.0f;

    long long mem_total_mb = 0;
    long long mem_used_mb = 0;
    int cpu_temp_c = 0;
    int gpu_temp_c = 0;

    uint64_t uptime_seconds = 0;

    struct DiskUsage {
        std::string mount_point;
        std::string device;
        uint64_t total_bytes = 0;
        uint64_t used_bytes = 0;
        float usage_pct = 0.0f;
    };
    std::vector<DiskUsage> disks;

    struct NetInterface {
        std::string name;
        uint64_t rx_bytes = 0;
        uint64_t tx_bytes = 0;
        uint64_t rx_bytes_prev = 0;
        uint64_t tx_bytes_prev = 0;
        float rx_rate_mbps = 0.0f;
        float tx_rate_mbps = 0.0f;
        bool up = false;
    };
    std::vector<NetInterface> net_interfaces;

    struct Alert {
        std::string severity; // "critical", "warning", "info"
        std::string category;
        std::string title;
        std::string detail;
        std::string timestamp;
    };
    std::vector<Alert> recent_alerts;

    int health_score = 100; // 0-100
};

class Dashboard {
public:
    Dashboard() = default;

    /// Sample all system metrics.
    void sample();

    /// Render the dashboard tab.
    void render();

    /// Get the latest health data.
    const SystemHealth& health() const { return health_; }

private:
    SystemHealth health_;
    std::chrono::steady_clock::time_point boot_time_;
    bool first_sample_ = true;

    void sample_cpu();
    void sample_memory();
    void sample_gpu();
    void sample_disks();
    void sample_network();
    void sample_uptime();
    void compute_health_score();

    void render_gauge(const char* label, float value, ImVec4 color, float radius);
    void render_disk_bars();
    void render_network_graph();
    void render_alerts();

    // CPU timing
    long long prev_cpu_active_ = 0;
    long long prev_cpu_total_ = 0;

    // Network history for graph
    static constexpr int NET_HISTORY = 60;
    float rx_history_[NET_HISTORY]{};
    float tx_history_[NET_HISTORY]{};
    int net_history_idx_ = 0;
};

} // namespace straylight::hub
