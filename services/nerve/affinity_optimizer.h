// services/nerve/affinity_optimizer.h
// Optimal IRQ placement using fabric topology, NUMA awareness, and thermal data.
#pragma once

#include "irq_mapper.h"

#include <straylight/result.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace straylight {

/// Device classification for IRQ routing policy.
enum class DeviceClass {
    GPU,
    NVMe,
    NIC,
    USB,
    Audio,
    VPU,
    Other
};

/// NUMA topology information for a CPU core.
struct CpuTopology {
    uint32_t cpu_id;
    uint32_t numa_node;
    uint32_t package_id;
    uint32_t core_id;        // Physical core (for HT siblings)
    uint64_t interrupt_load;  // Current interrupt count on this core
    float temperature_celsius;
    bool is_hot;              // Exceeds thermal threshold
};

/// Affinity recommendation from the optimizer.
struct AffinityRecommendation {
    uint32_t irq_number;
    std::string device_name;
    DeviceClass device_class;
    std::vector<uint32_t> recommended_cpus;
    std::vector<uint32_t> previous_cpus;
    std::string reason;
};

/// Optimizes IRQ-to-CPU affinity using NUMA topology, thermal data, and device class.
class AffinityOptimizer {
public:
    /// Run full optimization — analyze all IRQs and set optimal affinities.
    /// Returns the list of changes made.
    static Result<std::vector<AffinityRecommendation>, std::string> optimize();

    /// Generate recommendations without applying them.
    static Result<std::vector<AffinityRecommendation>, std::string> recommend();

    /// Rebalance IRQs away from a thermally hot core.
    static Result<std::vector<AffinityRecommendation>, std::string>
        rebalance_on_thermal(uint32_t hot_core);

    /// Generate a balance report — how evenly are interrupts distributed?
    static std::string balance_report();

private:
    /// Build the CPU topology map from sysfs.
    static std::vector<CpuTopology> build_topology();

    /// Read thermal data for all CPU cores.
    static void read_thermal_data(std::vector<CpuTopology>& topology);

    /// Read current interrupt load per CPU from /proc/interrupts.
    static void read_interrupt_load(std::vector<CpuTopology>& topology);

    /// Classify a device name into a DeviceClass.
    static DeviceClass classify_device(const std::string& device_name);

    /// Get NUMA node for a PCI device via sysfs.
    static int get_device_numa_node(const std::string& device_name);

    /// Get CPUs on a specific NUMA node.
    static std::vector<uint32_t> cpus_on_numa(const std::vector<CpuTopology>& topology,
                                               uint32_t numa_node);

    /// Find the CPU with lowest interrupt load from a candidate set.
    static uint32_t least_loaded_cpu(const std::vector<CpuTopology>& topology,
                                      const std::vector<uint32_t>& candidates);

    /// Find N least-loaded CPUs from a candidate set.
    static std::vector<uint32_t> least_loaded_cpus(const std::vector<CpuTopology>& topology,
                                                    const std::vector<uint32_t>& candidates,
                                                    uint32_t n);

    /// Get CPUs that are not currently marked as hot.
    static std::vector<uint32_t> cool_cpus(const std::vector<CpuTopology>& topology,
                                            const std::vector<uint32_t>& candidates);

    /// Thermal threshold in degrees Celsius.
    static constexpr float THERMAL_THRESHOLD = 85.0f;

    /// Interrupt storm threshold per second.
    static constexpr uint64_t STORM_THRESHOLD = 100000;
};

} // namespace straylight
