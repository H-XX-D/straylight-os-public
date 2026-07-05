/**
 * StrayLight Bridge Engine — Cross-machine shared memory.
 *
 * Processes on different nodes share memory regions transparently.
 * Local side uses shm_open + mmap; write faults are tracked via
 * userfaultfd/soft-dirty and synced to the remote node.
 */
#pragma once

#include "network_sync.h"
#include "page_tracker.h"
#include "straylight/result.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace straylight::bridge {

/// Unique bridge identifier.
using BridgeId = uint32_t;

/// Bridge descriptor.
struct BridgeDescriptor {
    BridgeId id;
    std::string region_name;
    std::string remote_host;
    uint16_t remote_port;
    size_t size;
    SyncMode sync_mode;
    bool encrypted;

    // Runtime state.
    void* local_addr = nullptr;
    int shm_fd = -1;
    std::chrono::steady_clock::time_point created_at;
    uint64_t total_syncs = 0;
    uint64_t total_bytes_synced = 0;
};

/// Bridge statistics.
struct BridgeStats {
    BridgeId id;
    std::string region_name;
    size_t region_size;
    uint64_t total_syncs;
    uint64_t total_pages_synced;
    uint64_t total_bytes_synced;
    uint64_t dirty_pages_current;
    double avg_sync_latency_ms;
    double uptime_seconds;
    SyncMode sync_mode;
    bool connected;
};

class BridgeEngine {
public:
    BridgeEngine();
    ~BridgeEngine();

    /// Create a named shared memory bridge to a remote host.
    Result<BridgeId, std::string> create_bridge(const std::string& remote_host,
                                                  const std::string& region_name,
                                                  size_t size,
                                                  SyncMode mode = SyncMode::Batched,
                                                  bool encrypted = false);

    /// Destroy a bridge and clean up resources.
    VoidResult<std::string> destroy_bridge(BridgeId id);

    /// Manually trigger a sync for a bridge.
    VoidResult<std::string> sync_bridge(BridgeId id);

    /// Get a pointer to the local shared memory for a bridge.
    Result<void*, std::string> get_local_addr(BridgeId id);

    /// List all active bridges.
    std::vector<BridgeDescriptor> list_bridges() const;

    /// Get statistics for a specific bridge.
    Result<BridgeStats, std::string> get_stats(BridgeId id) const;

    /// Start the background sync thread (for batched/immediate modes).
    void start_sync_thread();

    /// Stop the background sync thread.
    void stop_sync_thread();

    /// Set the default remote port for bridge connections.
    void set_default_port(uint16_t port) { default_port_ = port; }

private:
    struct BridgeState {
        BridgeDescriptor descriptor;
        PageTracker tracker;
        NetworkSync sync;
        std::thread sync_thread;
        std::atomic<bool> running{false};
    };

    std::map<BridgeId, std::unique_ptr<BridgeState>> bridges_;
    mutable std::mutex bridges_mutex_;
    BridgeId next_id_ = 1;
    uint16_t default_port_ = 9901;

    std::thread batch_thread_;
    std::atomic<bool> batch_running_{false};

    /// Background sync loop for batched mode bridges.
    void batch_sync_loop();

    /// Sync a single bridge.
    VoidResult<std::string> do_sync(BridgeState& state);
};

} // namespace straylight::bridge
