// services/swarm/swarm_daemon.h
#pragma once

#include <straylight/common.h>
#include <straylight/daemon.h>
#include "discovery.h"
#include "orchestrator.h"
#include "node_client.h"

#include <chrono>
#include <mutex>

namespace straylight {

/// Daemon that runs the Swarm subsystem: node discovery, heartbeat loop,
/// task orchestration. Exposes D-Bus interface org.straylight.Swarm1.
class SwarmDaemon : public DaemonBase {
public:
    Result<void, SLError> init(const Config& cfg) override;
    Result<void, SLError> tick() override;
    void shutdown() override;

    /// D-Bus method handlers.
    std::vector<SwarmNode> dbus_list_nodes() const;
    std::string dbus_submit_task(const std::string& command, const std::string& strategy);
    std::string dbus_task_status(const std::string& task_id) const;
    void dbus_cancel_task(const std::string& task_id);

private:
    /// Build the local node's SwarmNode from system info.
    SwarmNode build_self_node(const Config& cfg);

    /// Generate a UUID v4.
    static std::string generate_uuid();

    NodeDiscovery discovery_;
    NodeClient client_;
    std::unique_ptr<SwarmOrchestrator> orchestrator_;

    int heartbeat_interval_s_ = 10;
    int stale_timeout_s_ = 60;
    int task_poll_interval_s_ = 2;

    std::chrono::steady_clock::time_point last_heartbeat_;
    std::chrono::steady_clock::time_point last_task_poll_;
    std::chrono::steady_clock::time_point last_eviction_;

    mutable std::mutex mutex_;
};

} // namespace straylight
