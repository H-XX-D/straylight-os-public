#pragma once

#include <straylight/export.h>
#include <straylight/result.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace straylight::hw {

struct GpuDeviceInfo {
    uint32_t index;
    std::string name;           // e.g. "NVIDIA RTX 4090"
    std::string vendor;         // "nvidia", "amd", "intel"
    uint16_t pci_vendor;
    uint16_t pci_device;
    size_t vram_total;
    size_t vram_used;
    float temperature;          // Celsius, -1 if unavailable
    int p2p_peers;              // bitmask of P2P-capable peer GPUs
    std::string pci_slot;       // e.g. "0000:01:00.0"
};

enum class PlacementPolicy : uint8_t {
    RoundRobin = 0,
    LeastUsed = 1,
    Affinity = 2,     // stick to one GPU
    Mirror = 3,       // replicate across all
};

struct MgpuAllocation {
    uint64_t handle;
    uint32_t gpu_index;
    uint64_t gpu_addr;
    size_t size;
    std::vector<uint64_t> mirror_handles; // populated if mirrored
};

class STRAYLIGHT_EXPORT MultiGpuManager {
public:
    MultiGpuManager();
    ~MultiGpuManager();

    // Non-copyable, movable
    MultiGpuManager(const MultiGpuManager&) = delete;
    MultiGpuManager& operator=(const MultiGpuManager&) = delete;
    MultiGpuManager(MultiGpuManager&& other) noexcept;
    MultiGpuManager& operator=(MultiGpuManager&& other) noexcept;

    straylight::Result<void, std::string> discover();
    std::vector<GpuDeviceInfo> gpus() const;
    size_t gpu_count() const;

    straylight::Result<MgpuAllocation, std::string> allocate(
        size_t bytes, PlacementPolicy policy = PlacementPolicy::LeastUsed);
    straylight::Result<MgpuAllocation, std::string> allocate_on(
        uint32_t gpu_index, size_t bytes);
    straylight::Result<void, std::string> free(const MgpuAllocation& alloc);

    straylight::Result<void, std::string> p2p_copy(
        uint64_t src_handle, uint64_t dst_handle,
        size_t size, size_t offset = 0);
    straylight::Result<MgpuAllocation, std::string> mirror(
        uint64_t src_handle, uint32_t gpu_mask);

    straylight::Result<GpuDeviceInfo, std::string> gpu_stats(
        uint32_t gpu_index) const;
    bool has_p2p(uint32_t gpu_a, uint32_t gpu_b) const;

private:
    int vpu_fd_ = -1;
    std::vector<GpuDeviceInfo> gpus_;
    uint32_t rr_counter_ = 0;
    uint32_t last_gpu_ = 0;
    mutable std::mutex mu_;
};

} // namespace straylight::hw
