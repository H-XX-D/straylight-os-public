// services/mesh/gpu_pool.h
// StrayLight Mesh — Distributed GPU pool management.
#pragma once

#include <straylight/result.h>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace straylight {

// ---------------------------------------------------------------------------
// GPU descriptor
// ---------------------------------------------------------------------------

struct RemoteGpu {
    std::string host;
    uint32_t    gpu_index      = 0;
    std::string name;
    std::string vendor;
    size_t      vram_total     = 0;   // bytes
    size_t      vram_available = 0;   // bytes
    float       temperature    = 0.0f;
    float       utilization    = 0.0f; // 0.0 .. 1.0
    float       latency_ms     = 0.0f; // network latency
    bool        is_local       = false;
    bool        is_available   = true;

    std::chrono::steady_clock::time_point last_seen;
};

// ---------------------------------------------------------------------------
// Allocation
// ---------------------------------------------------------------------------

enum class PlacementPolicy : uint8_t {
    BestFit,       // GPU with least free VRAM that still fits
    LeastLoaded,   // GPU with lowest utilization
    LocalFirst,    // Prefer local GPUs, then remote
    RoundRobin,    // Distribute evenly
    Pinned,        // Specific GPU index
};

struct MeshAllocation {
    uint64_t    handle     = 0;
    std::string host;
    uint32_t    gpu_index  = 0;
    size_t      size_bytes = 0;
    bool        is_local   = false;
};

// ---------------------------------------------------------------------------
// GpuPool
// ---------------------------------------------------------------------------

class GpuPool {
public:
    GpuPool();
    ~GpuPool();

    /// Discover all GPUs on the local machine and across the swarm network.
    Result<void, std::string> discover();

    /// Return all known GPUs (local + remote).
    std::vector<RemoteGpu> all_gpus() const;

    /// Return GPUs below the utilization threshold and marked available.
    std::vector<RemoteGpu> available_gpus(float max_utilization = 0.9f) const;

    /// Allocate GPU memory according to the placement policy.
    Result<MeshAllocation, std::string> allocate(size_t bytes, PlacementPolicy policy);

    /// Free a previously allocated mesh allocation.
    Result<void, std::string> free(const MeshAllocation& alloc);

    /// Transfer data from one allocation to another (any GPU pair in the mesh).
    Result<void, std::string> transfer(const MeshAllocation& src,
                                        const MeshAllocation& dst);

    /// Submit a compute command to the best available GPU.
    Result<std::string, std::string> submit(const std::string& command,
                                             size_t vram_needed);

    /// Refresh stats for all known GPUs.
    Result<void, std::string> refresh_stats();

    /// Mark a specific GPU as unavailable (e.g., on node failure).
    void mark_unavailable(const std::string& host, uint32_t gpu_index);

    /// Mark a specific GPU as available again.
    void mark_available(const std::string& host, uint32_t gpu_index);

    /// Remove all GPUs associated with a host.
    void remove_host(const std::string& host);

    /// Get total and available VRAM across the entire mesh.
    void mesh_totals(size_t& total, size_t& available) const;

    /// Number of GPUs in the pool.
    size_t gpu_count() const;

private:
    /// Discover the local VPU allocator via /dev/straylight-vpu and sysfs.
    Result<void, std::string> discover_local();

    /// Discover remote GPUs via the Swarm daemon.
    Result<void, std::string> discover_remote();

    /// Select a GPU for allocation based on policy.
    Result<RemoteGpu*, std::string> select_gpu(size_t bytes, PlacementPolicy policy);

    /// Execute a VPU ioctl allocation on the local VPU allocator.
    Result<uint64_t, std::string> local_alloc(uint32_t gpu_index, size_t bytes);

    /// Execute an allocation on a remote GPU via the Remote agent.
    Result<uint64_t, std::string> remote_alloc(const std::string& host,
                                                uint32_t gpu_index, size_t bytes);

    /// Free allocation on the local VPU allocator.
    Result<void, std::string> local_free(uint32_t gpu_index, uint64_t handle);

    /// Free allocation on remote GPU.
    Result<void, std::string> remote_free(const std::string& host,
                                           uint32_t gpu_index, uint64_t handle);

    /// Transfer data between two local allocations when supported by the VPU ABI.
    Result<void, std::string> local_to_local_transfer(
        uint32_t src_gpu, uint64_t src_handle,
        uint32_t dst_gpu, uint64_t dst_handle, size_t bytes);

    /// Transfer data involving at least one remote GPU.
    Result<void, std::string> remote_transfer(
        const MeshAllocation& src, const MeshAllocation& dst);

    /// Execute a command on a remote host via the Remote agent.
    Result<std::string, std::string> remote_exec(const std::string& host,
                                                   const std::string& command);

    mutable std::mutex mutex_;
    std::vector<RemoteGpu> gpus_;
    uint64_t next_handle_ = 1;
    uint32_t round_robin_index_ = 0;
};

} // namespace straylight
