// bin/rhem/allocator.h
#pragma once

#include "discovery.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <straylight/result.h>

namespace straylight::rhem {

struct Allocation {
    uint32_t device_id;
    size_t memory_bytes;
    float compute_fraction;
    uint64_t lease_id;
};

/// Track and enforce resource allocations across heterogeneous devices.
class ResourceAllocator {
public:
    /// Initialize the allocator with the set of available devices.
    explicit ResourceAllocator(std::vector<Device> devices);

    /// Allocate resources on the preferred device type.
    /// Returns an Allocation with a unique lease_id.
    straylight::Result<Allocation, std::string> allocate(DeviceType preferred,
                                                          size_t mem,
                                                          float compute);

    /// Release a previously allocated lease.
    straylight::Result<void, std::string> release(uint64_t lease_id);

    /// Return all currently active allocations.
    std::vector<Allocation> active() const;

    /// Get device list.
    const std::vector<Device>& devices() const;

private:
    struct DeviceState {
        size_t used_memory = 0;
        float used_compute = 0.0f;
    };

    std::vector<Device> devices_;
    std::unordered_map<uint32_t, DeviceState> device_states_;
    std::unordered_map<uint64_t, Allocation> allocations_;
    uint64_t next_lease_id_ = 1;
    mutable std::mutex mu_;

    /// Find a suitable device of the given type with enough capacity.
    straylight::Result<uint32_t, std::string> find_device(DeviceType type,
                                                           size_t mem,
                                                           float compute);
};

} // namespace straylight::rhem
