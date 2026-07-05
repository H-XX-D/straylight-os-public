// services/mesh/mesh_monitor.h
// StrayLight Mesh — Pool health monitoring and rebalancing.
#pragma once

#include "gpu_pool.h"

#include <straylight/result.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace straylight {

// ---------------------------------------------------------------------------
// Alert types
// ---------------------------------------------------------------------------

enum class MeshAlertType : uint8_t {
    NodeJoined,
    NodeLeft,
    GpuOverheating,
    GpuOverloaded,
    PoolCapacityChanged,
    AllocationRebalanced,
};

struct MeshAlert {
    MeshAlertType type;
    std::string   host;
    uint32_t      gpu_index = 0;
    std::string   message;
    std::chrono::steady_clock::time_point timestamp;
};

// ---------------------------------------------------------------------------
// MeshMonitor
// ---------------------------------------------------------------------------

/// Monitors the GPU mesh health, detects failures, and emits D-Bus alerts.
class MeshMonitor {
public:
    explicit MeshMonitor(GpuPool& pool);
    ~MeshMonitor();

    /// Start the monitoring loop (spawns a background thread).
    Result<void, std::string> start(std::chrono::seconds interval = std::chrono::seconds(5));

    /// Stop the monitoring loop.
    void stop();

    /// Run one health check cycle (called by the daemon tick or the bg thread).
    void check_health();

    /// Get recent alerts (up to max_count, newest first).
    std::vector<MeshAlert> recent_alerts(size_t max_count = 50) const;

    /// Set a callback for alert emission (e.g., to D-Bus).
    using AlertCallback = std::function<void(const MeshAlert&)>;
    void set_alert_callback(AlertCallback cb);

    /// Get a summary of the mesh status as a formatted string.
    std::string status_summary() const;

    /// Thresholds
    void set_temperature_threshold(float celsius)   { temp_threshold_ = celsius; }
    void set_utilization_threshold(float pct)        { util_threshold_ = pct; }
    void set_stale_timeout(std::chrono::seconds sec) { stale_timeout_ = sec; }

private:
    /// Emit an alert: store it and invoke the callback.
    void emit_alert(MeshAlert alert);

    /// Detect newly joined or departed hosts.
    void detect_topology_changes();

    /// Check thermal and utilization limits.
    void check_thresholds();

    /// Evict GPUs from hosts not seen within the stale timeout.
    void evict_stale_hosts();

    GpuPool& pool_;

    std::atomic<bool> running_{false};
    std::thread monitor_thread_;
    std::chrono::seconds check_interval_{5};

    mutable std::mutex alert_mutex_;
    std::vector<MeshAlert> alerts_;
    static constexpr size_t MAX_ALERTS = 500;

    AlertCallback alert_callback_;

    // Track known hosts for join/leave detection
    std::unordered_set<std::string> known_hosts_;

    // Thresholds
    float temp_threshold_ = 90.0f;    // degrees C
    float util_threshold_ = 0.95f;    // 95%
    std::chrono::seconds stale_timeout_{30};
};

} // namespace straylight
