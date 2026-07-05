// bin/rhem/migration.cpp
#include "migration.h"

namespace straylight::rhem {

Migrator::Migrator(std::vector<Device> devices)
    : devices_(std::move(devices)) {}

const Device* Migrator::find_device(uint32_t id) const {
    for (const auto& dev : devices_) {
        if (dev.id == id) return &dev;
    }
    return nullptr;
}

float Migrator::estimate_bandwidth(DeviceType src_type, DeviceType dst_type) {
    // Bandwidth estimates in GB/s for common interconnects.
    // PCIe Gen4 x16 ~ 32 GB/s, NVLink ~ 600 GB/s, CPU-CPU ~ 50 GB/s

    if (src_type == dst_type) {
        // Same device type, assume high-bandwidth interconnect
        switch (src_type) {
        case DeviceType::CUDA: return 600.0f;  // NVLink peer
        case DeviceType::ROCm: return 200.0f;  // Infinity Fabric
        case DeviceType::CPU:  return 50.0f;    // NUMA interconnect
        case DeviceType::TPU:  return 300.0f;   // ICI
        case DeviceType::FPGA: return 25.0f;    // PCIe
        }
    }

    // Cross-type transfers go through PCIe
    bool involves_cpu = (src_type == DeviceType::CPU || dst_type == DeviceType::CPU);
    if (involves_cpu) {
        return 25.0f;  // PCIe Gen4 x16 effective throughput
    }

    // GPU-to-GPU across different vendors: CPU bounce
    return 12.0f;  // Effectively halved PCIe (src->CPU->dst)
}

straylight::Result<MigrationPlan, std::string> Migrator::plan(
    uint32_t src, uint32_t dst, size_t bytes) {

    if (src == dst) {
        return straylight::Result<MigrationPlan, std::string>::error(
            "Source and destination devices are the same");
    }

    const Device* src_dev = find_device(src);
    if (!src_dev) {
        return straylight::Result<MigrationPlan, std::string>::error(
            "Source device " + std::to_string(src) + " not found");
    }

    const Device* dst_dev = find_device(dst);
    if (!dst_dev) {
        return straylight::Result<MigrationPlan, std::string>::error(
            "Destination device " + std::to_string(dst) + " not found");
    }

    if (!src_dev->available) {
        return straylight::Result<MigrationPlan, std::string>::error(
            "Source device " + src_dev->name + " is not available");
    }
    if (!dst_dev->available) {
        return straylight::Result<MigrationPlan, std::string>::error(
            "Destination device " + dst_dev->name + " is not available");
    }

    // Check destination has enough memory
    if (bytes > dst_dev->memory_bytes) {
        return straylight::Result<MigrationPlan, std::string>::error(
            "Tensor (" + std::to_string(bytes) + " bytes) exceeds destination memory ("
            + std::to_string(dst_dev->memory_bytes) + " bytes)");
    }

    float bw_gbs = estimate_bandwidth(src_dev->type, dst_dev->type);
    // Convert GB/s to bytes/ms: 1 GB/s = 1e6 bytes/ms
    float bw_bytes_per_ms = bw_gbs * 1e6f;
    float estimated_ms = static_cast<float>(bytes) / bw_bytes_per_ms;

    // Add fixed overhead for setup (DMA descriptor, synchronization)
    estimated_ms += 0.1f;

    MigrationPlan mp;
    mp.src_device = src;
    mp.dst_device = dst;
    mp.tensor_bytes = bytes;
    mp.estimated_time_ms = estimated_ms;

    return straylight::Result<MigrationPlan, std::string>::ok(mp);
}

straylight::Result<void, std::string> Migrator::execute(const MigrationPlan& plan) {
    // Validate the plan references valid devices
    const Device* src_dev = find_device(plan.src_device);
    if (!src_dev) {
        return straylight::Result<void, std::string>::error(
            "Source device " + std::to_string(plan.src_device) + " not found");
    }

    const Device* dst_dev = find_device(plan.dst_device);
    if (!dst_dev) {
        return straylight::Result<void, std::string>::error(
            "Destination device " + std::to_string(plan.dst_device) + " not found");
    }

    if (!src_dev->available || !dst_dev->available) {
        return straylight::Result<void, std::string>::error(
            "One or both devices are unavailable");
    }

    // In a real implementation, this would:
    // 1. Pin source memory pages
    // 2. Allocate destination buffer
    // 3. Initiate DMA/RDMA transfer
    // 4. Wait for completion fence
    // 5. Verify checksum
    //
    // For now, we validate the plan is executable and return success.
    // The actual data movement would require device-specific driver APIs
    // (cudaMemcpyPeer, HSA memory copy, etc.)

    if (plan.tensor_bytes == 0) {
        return straylight::Result<void, std::string>::error(
            "Cannot migrate zero bytes");
    }

    return straylight::Result<void, std::string>::ok();
}

} // namespace straylight::rhem
