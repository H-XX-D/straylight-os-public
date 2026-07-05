// bin/rhem/policy.h
#pragma once

#include "discovery.h"

#include <cstdint>
#include <string>
#include <vector>

#include <straylight/result.h>

namespace straylight::rhem {

enum class PolicyType { RoundRobin, LeastLoaded, AffinityBased, PowerEfficient };

/// Device placement policy engine.
/// Selects the best device for a workload based on the chosen policy.
class PlacementPolicy {
public:
    /// Select a device from the given list according to the policy.
    /// Returns the device ID of the selected device.
    straylight::Result<uint32_t, std::string> select_device(
        const std::vector<Device>& devices,
        size_t mem_required,
        PolicyType policy);

    /// Reset round-robin counter.
    void reset();

private:
    uint32_t rr_counter_ = 0;

    straylight::Result<uint32_t, std::string> select_round_robin(
        const std::vector<Device>& devices, size_t mem_required);

    straylight::Result<uint32_t, std::string> select_least_loaded(
        const std::vector<Device>& devices, size_t mem_required);

    straylight::Result<uint32_t, std::string> select_affinity(
        const std::vector<Device>& devices, size_t mem_required);

    straylight::Result<uint32_t, std::string> select_power_efficient(
        const std::vector<Device>& devices, size_t mem_required);
};

/// Convert PolicyType to string.
inline std::string policy_type_str(PolicyType p) {
    switch (p) {
    case PolicyType::RoundRobin:      return "RoundRobin";
    case PolicyType::LeastLoaded:     return "LeastLoaded";
    case PolicyType::AffinityBased:   return "AffinityBased";
    case PolicyType::PowerEfficient:  return "PowerEfficient";
    }
    return "Unknown";
}

/// Parse PolicyType from string.
inline PolicyType policy_type_from_str(const std::string& s) {
    if (s == "round-robin" || s == "RoundRobin") return PolicyType::RoundRobin;
    if (s == "least-loaded" || s == "LeastLoaded") return PolicyType::LeastLoaded;
    if (s == "affinity" || s == "AffinityBased") return PolicyType::AffinityBased;
    if (s == "power-efficient" || s == "PowerEfficient") return PolicyType::PowerEfficient;
    return PolicyType::RoundRobin;
}

} // namespace straylight::rhem
