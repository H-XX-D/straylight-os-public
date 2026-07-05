// bin/rhem/migration.h
#pragma once

#include "discovery.h"

#include <cstdint>
#include <string>
#include <vector>

#include <straylight/result.h>

namespace straylight::rhem {

struct MigrationPlan {
    uint32_t src_device;
    uint32_t dst_device;
    size_t tensor_bytes;
    float estimated_time_ms;
};

/// Plan and execute tensor migration between heterogeneous devices.
class Migrator {
public:
    explicit Migrator(std::vector<Device> devices);

    /// Plan a migration of tensor_bytes from src to dst.
    /// Estimates transfer time based on interconnect bandwidth.
    straylight::Result<MigrationPlan, std::string> plan(uint32_t src, uint32_t dst,
                                                         size_t bytes);

    /// Execute a previously planned migration.
    /// (In practice this would issue DMA/RDMA commands; here we simulate it.)
    straylight::Result<void, std::string> execute(const MigrationPlan& plan);

private:
    std::vector<Device> devices_;

    /// Find a device by ID.
    const Device* find_device(uint32_t id) const;

    /// Estimate bandwidth in bytes/ms between two device types.
    static float estimate_bandwidth(DeviceType src_type, DeviceType dst_type);
};

} // namespace straylight::rhem
