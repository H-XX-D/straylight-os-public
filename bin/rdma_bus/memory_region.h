// bin/rdma_bus/memory_region.h
#pragma once

#include <straylight/result.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

struct ibv_mr;

namespace straylight::rdma {

/// Opaque handle to a registered memory region.
using RegionHandle = uint32_t;

/// Information about a registered memory region.
struct RegionInfo {
    void*    addr = nullptr;
    size_t   size = 0;
    uint32_t lkey = 0;
    uint32_t rkey = 0;
};

class VerbsContext; // forward declaration

/// Manages memory region registration/deregistration and metadata.
class MemoryRegionManager {
public:
    explicit MemoryRegionManager(VerbsContext& verbs);
    ~MemoryRegionManager();

    MemoryRegionManager(const MemoryRegionManager&) = delete;
    MemoryRegionManager& operator=(const MemoryRegionManager&) = delete;

    /// Register a contiguous memory region for RDMA.
    Result<RegionHandle, std::string> register_region(void* ptr, size_t size);

    /// Deregister a previously registered region.
    Result<void, std::string> deregister(RegionHandle handle);

    /// Get info for a registered region, or nullptr if not found.
    [[nodiscard]] const RegionInfo* get_info(RegionHandle handle) const;

    /// Number of currently registered regions.
    [[nodiscard]] size_t count() const noexcept;

private:
    VerbsContext& verbs_;

    struct ManagedRegion {
        struct ibv_mr* mr;
        RegionInfo     info;
    };

    std::unordered_map<RegionHandle, ManagedRegion> regions_;
    RegionHandle next_handle_ = 1;
};

} // namespace straylight::rdma
