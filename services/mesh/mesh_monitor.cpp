// services/mesh/mesh_monitor.cpp
// StrayLight Mesh — Pool monitoring implementation.
#include "mesh_monitor.h"

#include <straylight/log.h>

#include <algorithm>
#include <sstream>

namespace straylight {

MeshMonitor::MeshMonitor(GpuPool& pool)
    : pool_(pool) {}

MeshMonitor::~MeshMonitor() {
    stop();
}

// ---------------------------------------------------------------------------
// Start / Stop
// ---------------------------------------------------------------------------

Result<void, std::string> MeshMonitor::start(std::chrono::seconds interval) {
    if (running_.load()) {
        return Result<void, std::string>::error("Monitor already running");
    }

    check_interval_ = interval;
    running_.store(true);

    // Seed known hosts from current pool
    auto gpus = pool_.all_gpus();
    for (const auto& g : gpus) {
        known_hosts_.insert(g.host);
    }

    monitor_thread_ = std::thread([this]() {
        SL_INFO("mesh-monitor: health check thread started (interval={}s)",
                check_interval_.count());

        while (running_.load()) {
            check_health();

            // Sleep in small increments so we can exit promptly
            auto deadline = std::chrono::steady_clock::now() + check_interval_;
            while (running_.load() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
        }

        SL_INFO("mesh-monitor: health check thread stopped");
    });

    return Result<void, std::string>::ok();
}

void MeshMonitor::stop() {
    running_.store(false);
    if (monitor_thread_.joinable()) {
        monitor_thread_.join();
    }
}

// ---------------------------------------------------------------------------
// Health check
// ---------------------------------------------------------------------------

void MeshMonitor::check_health() {
    // Refresh GPU stats from all nodes
    pool_.refresh_stats();

    // Detect topology changes (nodes joining/leaving)
    detect_topology_changes();

    // Check thermal and utilization thresholds
    check_thresholds();

    // Evict stale nodes
    evict_stale_hosts();
}

// ---------------------------------------------------------------------------
// Topology changes
// ---------------------------------------------------------------------------

void MeshMonitor::detect_topology_changes() {
    auto gpus = pool_.all_gpus();

    std::unordered_set<std::string> current_hosts;
    for (const auto& g : gpus) {
        current_hosts.insert(g.host);
    }

    // Detect new hosts
    for (const auto& host : current_hosts) {
        if (known_hosts_.find(host) == known_hosts_.end()) {
            MeshAlert alert;
            alert.type      = MeshAlertType::NodeJoined;
            alert.host      = host;
            alert.message   = "Node joined the mesh: " + host;
            alert.timestamp = std::chrono::steady_clock::now();
            emit_alert(std::move(alert));
            SL_INFO("mesh-monitor: node joined: {}", host);
        }
    }

    // Detect departed hosts
    for (const auto& host : known_hosts_) {
        if (current_hosts.find(host) == current_hosts.end()) {
            MeshAlert alert;
            alert.type      = MeshAlertType::NodeLeft;
            alert.host      = host;
            alert.message   = "Node left the mesh: " + host;
            alert.timestamp = std::chrono::steady_clock::now();
            emit_alert(std::move(alert));
            SL_WARN("mesh-monitor: node departed: {}", host);
        }
    }

    // Detect capacity changes
    size_t total = 0, available = 0;
    pool_.mesh_totals(total, available);

    MeshAlert cap_alert;
    cap_alert.type      = MeshAlertType::PoolCapacityChanged;
    cap_alert.message   = "Pool: " + std::to_string(pool_.gpu_count()) + " GPUs, " +
                           std::to_string(total / (1024 * 1024 * 1024)) + " GiB total, " +
                           std::to_string(available / (1024 * 1024 * 1024)) + " GiB available";
    cap_alert.timestamp = std::chrono::steady_clock::now();

    // Only emit if topology actually changed
    if (current_hosts != known_hosts_) {
        emit_alert(std::move(cap_alert));
    }

    known_hosts_ = current_hosts;
}

// ---------------------------------------------------------------------------
// Threshold checks
// ---------------------------------------------------------------------------

void MeshMonitor::check_thresholds() {
    auto gpus = pool_.all_gpus();

    for (const auto& gpu : gpus) {
        if (!gpu.is_available) continue;

        // Temperature check
        if (gpu.temperature > temp_threshold_) {
            MeshAlert alert;
            alert.type      = MeshAlertType::GpuOverheating;
            alert.host      = gpu.host;
            alert.gpu_index = gpu.gpu_index;
            alert.message   = gpu.host + ":gpu" + std::to_string(gpu.gpu_index) +
                               " temperature " + std::to_string(static_cast<int>(gpu.temperature)) +
                               "C exceeds threshold " + std::to_string(static_cast<int>(temp_threshold_)) + "C";
            alert.timestamp = std::chrono::steady_clock::now();
            emit_alert(std::move(alert));
            SL_WARN("mesh-monitor: GPU overheating: {}:gpu{} at {}C",
                     gpu.host, gpu.gpu_index, gpu.temperature);
        }

        // Utilization check
        if (gpu.utilization > util_threshold_) {
            MeshAlert alert;
            alert.type      = MeshAlertType::GpuOverloaded;
            alert.host      = gpu.host;
            alert.gpu_index = gpu.gpu_index;
            alert.message   = gpu.host + ":gpu" + std::to_string(gpu.gpu_index) +
                               " utilization " + std::to_string(static_cast<int>(gpu.utilization * 100)) +
                               "% exceeds threshold " + std::to_string(static_cast<int>(util_threshold_ * 100)) + "%";
            alert.timestamp = std::chrono::steady_clock::now();
            emit_alert(std::move(alert));
        }
    }
}

// ---------------------------------------------------------------------------
// Stale host eviction
// ---------------------------------------------------------------------------

void MeshMonitor::evict_stale_hosts() {
    auto gpus = pool_.all_gpus();
    auto now = std::chrono::steady_clock::now();

    std::unordered_set<std::string> stale_hosts;
    for (const auto& gpu : gpus) {
        if (gpu.is_local) continue; // never evict local
        auto age = std::chrono::duration_cast<std::chrono::seconds>(now - gpu.last_seen);
        if (age > stale_timeout_) {
            stale_hosts.insert(gpu.host);
        }
    }

    for (const auto& host : stale_hosts) {
        pool_.mark_unavailable(host, 0); // mark all GPUs on host
        // Actually remove after an extended timeout (2x stale)
        bool all_stale = true;
        for (const auto& gpu : gpus) {
            if (gpu.host != host) continue;
            auto age = std::chrono::duration_cast<std::chrono::seconds>(now - gpu.last_seen);
            if (age <= stale_timeout_ * 2) {
                all_stale = false;
                break;
            }
        }
        if (all_stale) {
            pool_.remove_host(host);
            MeshAlert alert;
            alert.type      = MeshAlertType::NodeLeft;
            alert.host      = host;
            alert.message   = "Evicted stale host: " + host;
            alert.timestamp = now;
            emit_alert(std::move(alert));
        }
    }
}

// ---------------------------------------------------------------------------
// Alert management
// ---------------------------------------------------------------------------

void MeshMonitor::emit_alert(MeshAlert alert) {
    std::lock_guard<std::mutex> lock(alert_mutex_);

    if (alert_callback_) {
        alert_callback_(alert);
    }

    alerts_.push_back(std::move(alert));
    if (alerts_.size() > MAX_ALERTS) {
        alerts_.erase(alerts_.begin(),
                      alerts_.begin() + static_cast<long>(alerts_.size() - MAX_ALERTS));
    }
}

std::vector<MeshAlert> MeshMonitor::recent_alerts(size_t max_count) const {
    std::lock_guard<std::mutex> lock(alert_mutex_);

    size_t count = std::min(max_count, alerts_.size());
    std::vector<MeshAlert> result(alerts_.end() - static_cast<long>(count),
                                   alerts_.end());
    std::reverse(result.begin(), result.end());
    return result;
}

void MeshMonitor::set_alert_callback(AlertCallback cb) {
    alert_callback_ = std::move(cb);
}

// ---------------------------------------------------------------------------
// Status summary
// ---------------------------------------------------------------------------

std::string MeshMonitor::status_summary() const {
    auto gpus = pool_.all_gpus();

    size_t total_vram = 0, avail_vram = 0;
    int local_count = 0, remote_count = 0;
    int available_count = 0;
    float max_temp = 0.0f, max_util = 0.0f;

    std::unordered_set<std::string> hosts;

    for (const auto& gpu : gpus) {
        hosts.insert(gpu.host);
        total_vram += gpu.vram_total;
        avail_vram += gpu.vram_available;
        if (gpu.is_local)  local_count++;
        else                remote_count++;
        if (gpu.is_available) available_count++;
        max_temp = std::max(max_temp, gpu.temperature);
        max_util = std::max(max_util, gpu.utilization);
    }

    std::ostringstream out;
    out << "StrayLight Mesh Status\n";
    out << "======================\n";
    out << "Nodes:        " << hosts.size() << "\n";
    out << "GPUs:         " << gpus.size()
        << " (" << local_count << " local, " << remote_count << " remote)\n";
    out << "Available:    " << available_count << "/" << gpus.size() << "\n";
    out << "Total VRAM:   " << (total_vram / (1024 * 1024 * 1024)) << " GiB\n";
    out << "Free VRAM:    " << (avail_vram / (1024 * 1024 * 1024)) << " GiB\n";
    out << "Max Temp:     " << static_cast<int>(max_temp) << " C\n";
    out << "Max Util:     " << static_cast<int>(max_util * 100) << "%\n";

    auto alerts = recent_alerts(5);
    if (!alerts.empty()) {
        out << "\nRecent Alerts:\n";
        for (const auto& a : alerts) {
            out << "  - " << a.message << "\n";
        }
    }

    return out.str();
}

} // namespace straylight
