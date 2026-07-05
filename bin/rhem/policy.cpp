// bin/rhem/policy.cpp
#include "policy.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace straylight::rhem {

void PlacementPolicy::reset() {
    rr_counter_ = 0;
}

straylight::Result<uint32_t, std::string> PlacementPolicy::select_round_robin(
    const std::vector<Device>& devices, size_t mem_required) {

    // Filter eligible devices (available + enough memory)
    std::vector<const Device*> eligible;
    for (const auto& dev : devices) {
        if (dev.available && dev.memory_bytes >= mem_required) {
            eligible.push_back(&dev);
        }
    }

    if (eligible.empty()) {
        return straylight::Result<uint32_t, std::string>::error(
            "No eligible device with " + std::to_string(mem_required) + " bytes");
    }

    // Round-robin over eligible devices
    uint32_t idx = rr_counter_ % static_cast<uint32_t>(eligible.size());
    rr_counter_++;

    return straylight::Result<uint32_t, std::string>::ok(eligible[idx]->id);
}

straylight::Result<uint32_t, std::string> PlacementPolicy::select_least_loaded(
    const std::vector<Device>& devices, size_t mem_required) {

    // Select the device with the most available memory (proxy for "least loaded").
    const Device* best = nullptr;
    size_t best_avail = 0;

    for (const auto& dev : devices) {
        if (!dev.available || dev.memory_bytes < mem_required) continue;

        // Use memory_bytes as a proxy for total capacity.
        // In a real system, we'd track actual usage per device.
        // Here, we prefer the device with the largest memory (most headroom).
        size_t avail = dev.memory_bytes;
        if (!best || avail > best_avail) {
            best = &dev;
            best_avail = avail;
        }
    }

    if (!best) {
        return straylight::Result<uint32_t, std::string>::error(
            "No eligible device with " + std::to_string(mem_required) + " bytes");
    }

    return straylight::Result<uint32_t, std::string>::ok(best->id);
}

straylight::Result<uint32_t, std::string> PlacementPolicy::select_affinity(
    const std::vector<Device>& devices, size_t mem_required) {

    // Affinity-based: prefer accelerators (CUDA > ROCm > TPU > FPGA > CPU).
    // Among same type, prefer higher compute TFLOPS.
    const Device* best = nullptr;

    auto type_priority = [](DeviceType t) -> int {
        switch (t) {
        case DeviceType::CUDA: return 5;
        case DeviceType::ROCm: return 4;
        case DeviceType::TPU:  return 3;
        case DeviceType::FPGA: return 2;
        case DeviceType::CPU:  return 1;
        }
        return 0;
    };

    for (const auto& dev : devices) {
        if (!dev.available || dev.memory_bytes < mem_required) continue;

        if (!best) {
            best = &dev;
            continue;
        }

        int dev_pri = type_priority(dev.type);
        int best_pri = type_priority(best->type);

        if (dev_pri > best_pri) {
            best = &dev;
        } else if (dev_pri == best_pri && dev.compute_tflops > best->compute_tflops) {
            best = &dev;
        }
    }

    if (!best) {
        return straylight::Result<uint32_t, std::string>::error(
            "No eligible device with " + std::to_string(mem_required) + " bytes");
    }

    return straylight::Result<uint32_t, std::string>::ok(best->id);
}

straylight::Result<uint32_t, std::string> PlacementPolicy::select_power_efficient(
    const std::vector<Device>& devices, size_t mem_required) {

    // Power-efficient: prefer the device with the best TFLOPS-per-watt ratio.
    // Approximate power consumption by device type:
    //   CPU:  150W, CUDA: 300W, ROCm: 250W, TPU: 200W, FPGA: 75W
    auto estimated_watts = [](DeviceType t) -> float {
        switch (t) {
        case DeviceType::CPU:  return 150.0f;
        case DeviceType::CUDA: return 300.0f;
        case DeviceType::ROCm: return 250.0f;
        case DeviceType::TPU:  return 200.0f;
        case DeviceType::FPGA: return 75.0f;
        }
        return 200.0f;
    };

    const Device* best = nullptr;
    float best_efficiency = -1.0f;

    for (const auto& dev : devices) {
        if (!dev.available || dev.memory_bytes < mem_required) continue;

        float watts = estimated_watts(dev.type);
        float efficiency = dev.compute_tflops / watts;  // TFLOPS per watt

        if (efficiency > best_efficiency) {
            best = &dev;
            best_efficiency = efficiency;
        }
    }

    if (!best) {
        return straylight::Result<uint32_t, std::string>::error(
            "No eligible device with " + std::to_string(mem_required) + " bytes");
    }

    return straylight::Result<uint32_t, std::string>::ok(best->id);
}

straylight::Result<uint32_t, std::string> PlacementPolicy::select_device(
    const std::vector<Device>& devices, size_t mem_required, PolicyType policy) {

    if (devices.empty()) {
        return straylight::Result<uint32_t, std::string>::error("No devices available");
    }

    switch (policy) {
    case PolicyType::RoundRobin:
        return select_round_robin(devices, mem_required);
    case PolicyType::LeastLoaded:
        return select_least_loaded(devices, mem_required);
    case PolicyType::AffinityBased:
        return select_affinity(devices, mem_required);
    case PolicyType::PowerEfficient:
        return select_power_efficient(devices, mem_required);
    }

    return straylight::Result<uint32_t, std::string>::error("Unknown policy type");
}

} // namespace straylight::rhem
