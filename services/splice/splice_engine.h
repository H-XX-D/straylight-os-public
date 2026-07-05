// services/splice/splice_engine.h
// Core splice engine — manages zero-copy shared-memory sessions between processes.
#pragma once

#include "splice_ring.h"

#include <straylight/result.h>
#include <straylight/error.h>

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace straylight {

/// Describes a VPU slab allocation used for a splice region.
struct VpuSlabAlloc {
    int slab_order{0};         // VPU slab order (log2 of page count)
    int block_idx{-1};         // Block index within the slab
    uint64_t phys_addr{0};     // Physical address of the allocated block
    uint64_t page_count{0};    // Number of pages in the allocation
};

/// A splice session: zero-copy shared memory region mapped into two processes.
struct SpliceSession {
    uint64_t session_id{0};
    pid_t producer_pid{0};
    pid_t consumer_pid{0};
    std::string region_name;
    uint64_t size{0};                 // Total shared region size
    VpuSlabAlloc slab;

    // Mapped addresses in each process
    uint64_t producer_addr{0};
    uint64_t consumer_addr{0};

    // Our local mapping (daemon uses this to initialize the ring header)
    void* local_mapping{nullptr};

    // Metrics
    uint64_t bytes_transferred{0};
    uint64_t push_count{0};
    uint64_t pop_count{0};
    std::chrono::steady_clock::time_point created_at;
    std::chrono::steady_clock::time_point last_activity;

    // Derived stats
    double throughput_mbps{0.0};
    double avg_latency_us{0.0};
};

/// Throughput snapshot for a splice session.
struct SpliceStats {
    uint64_t session_id{0};
    uint64_t bytes_transferred{0};
    uint64_t push_count{0};
    uint64_t pop_count{0};
    double throughput_mbps{0.0};
    double avg_latency_us{0.0};
    uint64_t ring_available{0};
    uint64_t ring_capacity{0};
    double fill_ratio{0.0};
    double uptime_seconds{0.0};
};

/// The splice engine manages VPU-backed shared memory regions between process pairs.
class SpliceEngine {
public:
    SpliceEngine();
    ~SpliceEngine();

    /// Create a new splice session between two processes.
    /// Allocates a VPU slab block, maps the physical pages into both processes,
    /// and initializes the lock-free ring buffer at the start of the region.
    Result<uint64_t, SLError> create_splice(pid_t producer_pid, pid_t consumer_pid,
                                             uint64_t size);

    /// Destroy an existing splice session.
    /// Unmaps the region from both processes and frees the VPU slab block.
    Result<void, SLError> destroy_splice(uint64_t session_id);

    /// List all active splice sessions with current metrics.
    std::vector<SpliceSession> list_splices() const;

    /// Get detailed stats for a single session.
    Result<SpliceStats, SLError> get_stats(uint64_t session_id) const;

    /// Update throughput metrics for all sessions (called periodically by the daemon).
    void update_metrics();

private:
    /// Compute the slab order needed for a given size.
    static int compute_slab_order(uint64_t size);

    /// Allocate a VPU slab block via ioctl.
    Result<VpuSlabAlloc, SLError> vpu_alloc_block(int slab_order);

    /// Free a VPU slab block.
    void vpu_free_block(const VpuSlabAlloc& alloc);

    /// Map a physical region into a target process's address space.
    Result<uint64_t, SLError> map_into_process(pid_t pid, uint64_t phys_addr,
                                                uint64_t size);

    /// Unmap a region from a target process.
    void unmap_from_process(pid_t pid, uint64_t addr, uint64_t size);

    /// Map the region locally so the daemon can write the ring header.
    Result<void*, SLError> map_local(uint64_t phys_addr, uint64_t size);

    /// Unmap a local mapping.
    void unmap_local(void* addr, uint64_t size);

    mutable std::mutex mutex_;
    std::map<uint64_t, SpliceSession> sessions_;
    uint64_t next_session_id_{1};
    int vpu_fd_{-1};

    // Metrics tracking
    std::chrono::steady_clock::time_point last_metric_update_;
    std::map<uint64_t, uint64_t> prev_bytes_; // session_id -> bytes at last sample
};

} // namespace straylight
