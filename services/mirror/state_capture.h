/**
 * StrayLight Mirror State Capture — System state serialization.
 *
 * Captures running process lists, daemon states, VPU metadata,
 * and network configuration for live system cloning.
 */
#pragma once

#include "straylight/result.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace straylight::mirror {

/// Captured process information.
struct ProcessInfo {
    int pid;
    int ppid;
    std::string name;
    std::string cmdline;
    std::string state;          // R, S, D, Z, etc.
    uint64_t rss_kb;
    std::vector<std::string> open_files;
    std::vector<std::string> network_connections;
};

/// Captured daemon state (DaemonBase snapshot).
struct DaemonSnapshot {
    std::string name;
    int pid;
    std::string config_json;    // Serialized configuration
    std::string runtime_json;   // Serialized runtime state
    bool running;
};

/// VPU slab allocation metadata.
struct VpuSlabInfo {
    uint32_t slab_id;
    uint64_t base_offset;
    uint64_t size_bytes;
    bool allocated;
    std::string owner;          // Process name that owns the allocation
    uint32_t ref_count;
};

/// VPU state metadata (NOT actual VRAM — just metadata for rebuild).
struct VpuState {
    uint64_t total_vram_bytes;
    uint64_t used_vram_bytes;
    std::vector<VpuSlabInfo> slabs;
    std::string slab_bitmap;    // Hex-encoded slab allocation bitmap
};

/// Network configuration snapshot.
struct NetworkState {
    struct Interface {
        std::string name;
        std::string ipv4_addr;
        std::string ipv6_addr;
        std::string mac_addr;
        bool up;
        int mtu;
    };

    struct Route {
        std::string destination;
        std::string gateway;
        std::string interface;
        int metric;
    };

    struct IptablesRule {
        std::string chain;      // INPUT, OUTPUT, FORWARD
        std::string rule;       // Full rule text
    };

    struct MeshNode {
        std::string node_id;
        std::string address;
        bool reachable;
        int latency_ms;
    };

    std::vector<Interface> interfaces;
    std::vector<Route> routes;
    std::vector<IptablesRule> iptables_rules;
    std::vector<MeshNode> mesh_nodes;
};

/// Complete system state snapshot.
struct SystemState {
    uint64_t capture_timestamp_ms;
    std::string hostname;
    std::string kernel_version;
    std::vector<ProcessInfo> processes;
    std::vector<DaemonSnapshot> daemons;
    VpuState vpu_state;
    NetworkState network_state;

    /// Serialize the full state to a binary blob.
    std::vector<uint8_t> serialize() const;

    /// Deserialize from a binary blob.
    static Result<SystemState, std::string> deserialize(const std::vector<uint8_t>& data);
};

class StateCapture {
public:
    /// Capture the running process list.
    static Result<std::vector<ProcessInfo>, std::string> capture_process_list();

    /// Capture all DaemonBase daemon snapshots.
    static Result<std::vector<DaemonSnapshot>, std::string> capture_service_state();

    /// Capture VPU slab metadata.
    static Result<VpuState, std::string> capture_vpu_state();

    /// Capture full network configuration.
    static Result<NetworkState, std::string> capture_network_state();

    /// Capture the complete system state.
    static Result<SystemState, std::string> capture_all();
};

} // namespace straylight::mirror
