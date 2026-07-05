// bin/rhem/allocator.cpp
#include "allocator.h"

#include <algorithm>

namespace straylight::rhem {

ResourceAllocator::ResourceAllocator(std::vector<Device> devices)
    : devices_(std::move(devices)) {
    for (const auto& dev : devices_) {
        device_states_[dev.id] = DeviceState{0, 0.0f};
    }
}

straylight::Result<uint32_t, std::string> ResourceAllocator::find_device(
    DeviceType type, size_t mem, float compute) {

    // Find the first available device of the requested type with enough capacity
    for (const auto& dev : devices_) {
        if (dev.type != type || !dev.available) continue;

        auto it = device_states_.find(dev.id);
        if (it == device_states_.end()) continue;

        size_t avail_mem = dev.memory_bytes - it->second.used_memory;
        float avail_compute = 1.0f - it->second.used_compute;

        if (avail_mem >= mem && avail_compute >= compute) {
            return straylight::Result<uint32_t, std::string>::ok(dev.id);
        }
    }

    return straylight::Result<uint32_t, std::string>::error(
        "No " + device_type_str(type) + " device with sufficient capacity "
        "(need " + std::to_string(mem) + " bytes, " +
        std::to_string(compute) + " compute fraction)");
}

straylight::Result<Allocation, std::string> ResourceAllocator::allocate(
    DeviceType preferred, size_t mem, float compute) {

    std::lock_guard<std::mutex> lock(mu_);

    if (compute < 0.0f || compute > 1.0f) {
        return straylight::Result<Allocation, std::string>::error(
            "Compute fraction must be in [0, 1]");
    }

    auto dev_result = find_device(preferred, mem, compute);
    if (!dev_result.has_value()) {
        return straylight::Result<Allocation, std::string>::error(dev_result.error());
    }

    uint32_t dev_id = dev_result.value();

    Allocation alloc;
    alloc.device_id = dev_id;
    alloc.memory_bytes = mem;
    alloc.compute_fraction = compute;
    alloc.lease_id = next_lease_id_++;

    // Update device state
    device_states_[dev_id].used_memory += mem;
    device_states_[dev_id].used_compute += compute;

    allocations_[alloc.lease_id] = alloc;

    return straylight::Result<Allocation, std::string>::ok(alloc);
}

straylight::Result<void, std::string> ResourceAllocator::release(uint64_t lease_id) {
    std::lock_guard<std::mutex> lock(mu_);

    auto it = allocations_.find(lease_id);
    if (it == allocations_.end()) {
        return straylight::Result<void, std::string>::error(
            "Lease " + std::to_string(lease_id) + " not found");
    }

    const Allocation& alloc = it->second;

    auto ds = device_states_.find(alloc.device_id);
    if (ds != device_states_.end()) {
        ds->second.used_memory = (ds->second.used_memory > alloc.memory_bytes)
                                     ? ds->second.used_memory - alloc.memory_bytes
                                     : 0;
        ds->second.used_compute = (ds->second.used_compute > alloc.compute_fraction)
                                      ? ds->second.used_compute - alloc.compute_fraction
                                      : 0.0f;
    }

    allocations_.erase(it);
    return straylight::Result<void, std::string>::ok();
}

std::vector<Allocation> ResourceAllocator::active() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<Allocation> result;
    result.reserve(allocations_.size());
    for (const auto& [id, alloc] : allocations_) {
        result.push_back(alloc);
    }
    return result;
}

const std::vector<Device>& ResourceAllocator::devices() const {
    return devices_;
}

} // namespace straylight::rhem
