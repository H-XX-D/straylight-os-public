// services/swarm/discovery.h
#pragma once

#include <straylight/result.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace straylight {

/// Information about a single swarm node, carried in heartbeat packets
/// and maintained in the node registry.
struct SwarmNode {
    std::string node_id;        // UUID
    std::string hostname;
    std::string ip_address;
    uint16_t    port = 7700;    // straylight-remote TLS port

    // Resource summary
    int gpu_count        = 0;
    uint64_t vram_total  = 0;   // bytes
    uint64_t vram_free   = 0;
    int cpu_cores        = 0;
    uint64_t mem_total   = 0;
    uint64_t mem_free    = 0;

    // Health
    std::chrono::steady_clock::time_point last_seen;
    double latency_ms    = 0.0;
    double load_1m       = 0.0;
    bool   is_self       = false;
};

/// Wire format for multicast heartbeat.
/// Sent as a simple text protocol:
///   STRAYLIGHT-HB\n
///   node_id=<uuid>\n
///   hostname=<name>\n
///   ip=<addr>\n
///   port=<n>\n
///   gpus=<n>\n
///   vram_total=<bytes>\n
///   vram_free=<bytes>\n
///   cpu_cores=<n>\n
///   mem_total=<bytes>\n
///   mem_free=<bytes>\n
///   load=<float>\n
struct HeartbeatPacket {
    SwarmNode node;

    /// Serialize to wire format.
    std::string serialize() const;

    /// Deserialize from wire format. Returns error string on failure.
    static Result<HeartbeatPacket, std::string> deserialize(const std::string& data);
};

/// Discovers and tracks StrayLight nodes on the local network.
///
/// Discovery uses two mechanisms:
/// 1. mDNS/DNS-SD: announces `_straylight._tcp` service via Avahi or systemd-resolved
/// 2. UDP multicast: heartbeat on 224.0.0.42:7742 every 10 seconds
///
/// The registry tracks all known nodes with health info and evicts nodes
/// that haven't sent a heartbeat in 60 seconds.
class NodeDiscovery {
public:
    NodeDiscovery();
    ~NodeDiscovery();

    /// Initialize discovery with the local node's info.
    Result<void, std::string> start(const SwarmNode& self);

    /// Stop discovery threads and close sockets.
    void stop();

    /// Send one heartbeat immediately (called from tick loop).
    void send_heartbeat();

    /// Process any pending incoming heartbeats (non-blocking).
    void receive_heartbeats();

    /// Evict nodes not seen within the timeout.
    void evict_stale_nodes(std::chrono::seconds timeout = std::chrono::seconds(60));

    /// Return a snapshot of all known nodes (including self).
    std::vector<SwarmNode> nodes() const;

    /// Look up a specific node by ID.
    const SwarmNode* find_node(const std::string& node_id) const;

    /// Number of discovered nodes.
    size_t node_count() const;

private:
    /// Create and bind the multicast socket.
    Result<void, std::string> init_multicast_socket();

    /// Register mDNS/DNS-SD service.
    void register_mdns_service();

    /// Unregister mDNS/DNS-SD service.
    void unregister_mdns_service();

    SwarmNode self_;
    int mcast_fd_ = -1;                          // multicast UDP socket
    static constexpr const char* MCAST_GROUP = "224.0.0.42";
    static constexpr uint16_t MCAST_PORT = 7742;

    mutable std::mutex registry_mutex_;
    std::unordered_map<std::string, SwarmNode> registry_;  // node_id -> node

    std::atomic<bool> running_{false};
};

} // namespace straylight
