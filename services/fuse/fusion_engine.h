/**
 * StrayLight Fuse — Fusion Engine (header)
 *
 * Manages fusion sessions between cooperating processes.
 * A fusion session creates shared memory regions between two PIDs,
 * enabling zero-copy IPC through direct memory access.
 */
#pragma once

#include "shared_region.h"
#include "straylight/result.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace straylight::fuse {

/** Performance metrics for a fusion session. */
struct FusionMetrics {
    uint64_t bytes_transferred{0};
    uint64_t messages_exchanged{0};
    double   avg_latency_us{0.0};       // microseconds
    double   throughput_mbps{0.0};       // MB/s
    std::chrono::steady_clock::time_point started_at;
    std::chrono::steady_clock::time_point last_activity;
};

/** Represents a fusion session between two processes. */
struct FusionSession {
    std::string session_id;
    pid_t       pid1{0};
    pid_t       pid2{0};
    std::vector<std::string> region_ids;  // shared regions in this session
    size_t      total_shared_bytes{0};
    bool        active{false};
    FusionMetrics metrics;
};

class FusionEngine {
public:
    explicit FusionEngine(SharedRegionManager& region_mgr);

    /**
     * Create a new fusion session between two processes.
     * Sets up shared memory regions accessible by both PIDs.
     *
     * @param pid1           First process
     * @param pid2           Second process
     * @param shared_regions List of region sizes to create
     * @return Session ID on success
     */
    Result<std::string, std::string> create_session(
        pid_t pid1, pid_t pid2,
        const std::vector<size_t>& shared_regions);

    /**
     * Destroy a fusion session. Unmaps all shared regions.
     */
    VoidResult<> destroy_session(const std::string& session_id);

    /**
     * Get info about a session.
     */
    Result<FusionSession, std::string> get_session(
        const std::string& session_id) const;

    /**
     * List all active sessions.
     */
    std::vector<FusionSession> list_sessions() const;

    /**
     * Get aggregated stats across all sessions.
     */
    struct AggregateStats {
        size_t   active_sessions{0};
        size_t   total_regions{0};
        size_t   total_shared_bytes{0};
        uint64_t total_messages{0};
    };
    AggregateStats get_stats() const;

    /**
     * Check if a process is part of any fusion session.
     */
    bool is_fused(pid_t pid) const;

    /**
     * Clean up sessions where one or both processes have died.
     * Called periodically by the monitor.
     */
    void reap_dead_sessions();

private:
    SharedRegionManager& region_mgr_;
    mutable std::mutex mu_;
    std::unordered_map<std::string, FusionSession> sessions_;
    uint64_t session_counter_{0};

    /** Verify a PID is alive. */
    bool process_alive(pid_t pid) const;

    /** Read address space info from /proc/PID/maps. */
    Result<std::vector<std::pair<uint64_t,uint64_t>>, std::string>
        read_address_space(pid_t pid) const;
};

} // namespace straylight::fuse
