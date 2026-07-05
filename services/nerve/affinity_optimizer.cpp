// services/nerve/affinity_optimizer.cpp
#include "affinity_optimizer.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <sstream>

#include <unistd.h>

namespace straylight {

std::vector<CpuTopology> AffinityOptimizer::build_topology() {
    uint32_t num_cpus = static_cast<uint32_t>(::sysconf(_SC_NPROCESSORS_ONLN));
    std::vector<CpuTopology> topology;
    topology.reserve(num_cpus);

    for (uint32_t i = 0; i < num_cpus; ++i) {
        CpuTopology cpu;
        cpu.cpu_id = i;
        cpu.numa_node = 0;
        cpu.package_id = 0;
        cpu.core_id = i;
        cpu.interrupt_load = 0;
        cpu.temperature_celsius = 0.0f;
        cpu.is_hot = false;

        std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(i);

        // Read NUMA node
        std::string numa_path = base + "/topology/physical_package_id";
        std::ifstream numa_file(numa_path);
        if (numa_file.is_open()) {
            numa_file >> cpu.package_id;
        }

        // Try the actual NUMA node
        std::string node_dir = "/sys/devices/system/node";
        if (std::filesystem::exists(node_dir)) {
            for (const auto& entry : std::filesystem::directory_iterator(node_dir)) {
                std::string name = entry.path().filename().string();
                if (name.rfind("node", 0) == 0) {
                    auto cpumap_path = entry.path() / "cpumap";
                    std::ifstream cpumap(cpumap_path);
                    if (cpumap.is_open()) {
                        std::string mask;
                        std::getline(cpumap, mask);
                        auto cpus = IrqMapper::mask_to_cpus(mask);
                        for (uint32_t c : cpus) {
                            if (c == i) {
                                cpu.numa_node = static_cast<uint32_t>(
                                    std::stoul(name.substr(4)));
                                break;
                            }
                        }
                    }
                }
            }
        }

        // Read core_id
        std::string core_path = base + "/topology/core_id";
        std::ifstream core_file(core_path);
        if (core_file.is_open()) {
            core_file >> cpu.core_id;
        }

        topology.push_back(cpu);
    }

    return topology;
}

void AffinityOptimizer::read_thermal_data(std::vector<CpuTopology>& topology) {
    // Read from /sys/class/thermal/thermal_zone*/
    std::string thermal_base = "/sys/class/thermal";
    if (!std::filesystem::exists(thermal_base)) return;

    for (const auto& entry : std::filesystem::directory_iterator(thermal_base)) {
        std::string name = entry.path().filename().string();
        if (name.rfind("thermal_zone", 0) != 0) continue;

        // Read type to identify CPU thermal zones
        std::ifstream type_file(entry.path() / "type");
        if (!type_file.is_open()) continue;
        std::string type;
        std::getline(type_file, type);

        // CPU thermal zones have types like "x86_pkg_temp", "coretemp", etc.
        bool is_cpu_thermal = (type.find("cpu") != std::string::npos ||
                               type.find("core") != std::string::npos ||
                               type.find("pkg") != std::string::npos ||
                               type.find("x86") != std::string::npos);
        if (!is_cpu_thermal) continue;

        std::ifstream temp_file(entry.path() / "temp");
        if (!temp_file.is_open()) continue;
        int64_t millideg = 0;
        temp_file >> millideg;
        float temp_c = static_cast<float>(millideg) / 1000.0f;

        // Try to figure out which CPU(s) this zone covers
        // Often there's one zone per package; distribute temp to all CPUs on that package
        // For now, extract zone number as a heuristic
        uint32_t zone_num = 0;
        try { zone_num = static_cast<uint32_t>(std::stoul(name.substr(12))); }
        catch (...) { continue; }

        // Apply to matching CPUs
        for (auto& cpu : topology) {
            if (cpu.package_id == zone_num || topology.size() <= 8) {
                cpu.temperature_celsius = std::max(cpu.temperature_celsius, temp_c);
                cpu.is_hot = (cpu.temperature_celsius >= THERMAL_THRESHOLD);
            }
        }
    }

    // Also check hwmon for per-core temperatures
    std::string hwmon_base = "/sys/class/hwmon";
    if (!std::filesystem::exists(hwmon_base)) return;

    for (const auto& hwmon_entry : std::filesystem::directory_iterator(hwmon_base)) {
        std::ifstream name_file(hwmon_entry.path() / "name");
        if (!name_file.is_open()) continue;
        std::string hwmon_name;
        std::getline(name_file, hwmon_name);
        if (hwmon_name != "coretemp") continue;

        // Read temp*_input files
        for (const auto& file_entry : std::filesystem::directory_iterator(hwmon_entry.path())) {
            std::string fname = file_entry.path().filename().string();
            if (fname.rfind("temp", 0) == 0 && fname.find("_input") != std::string::npos) {
                std::ifstream tf(file_entry.path());
                int64_t millideg = 0;
                tf >> millideg;
                float temp_c = static_cast<float>(millideg) / 1000.0f;

                // Extract sensor index
                uint32_t idx = 0;
                try {
                    idx = static_cast<uint32_t>(std::stoul(fname.substr(4))) - 1;
                } catch (...) { continue; }

                if (idx < topology.size()) {
                    topology[idx].temperature_celsius = temp_c;
                    topology[idx].is_hot = (temp_c >= THERMAL_THRESHOLD);
                }
            }
        }
    }
}

void AffinityOptimizer::read_interrupt_load(std::vector<CpuTopology>& topology) {
    std::ifstream proc_ints("/proc/interrupts");
    if (!proc_ints.is_open()) return;

    std::string header;
    std::getline(proc_ints, header);

    // Count CPUs from header
    uint32_t num_cpus = 0;
    {
        std::istringstream hss(header);
        std::string tok;
        while (hss >> tok) {
            if (tok.rfind("CPU", 0) == 0) ++num_cpus;
        }
    }

    std::string line;
    while (std::getline(proc_ints, line)) {
        std::istringstream iss(line);
        std::string irq_str;
        iss >> irq_str;

        for (uint32_t i = 0; i < num_cpus && i < topology.size(); ++i) {
            uint64_t count = 0;
            if (iss >> count) {
                topology[i].interrupt_load += count;
            }
        }
    }
}

DeviceClass AffinityOptimizer::classify_device(const std::string& device_name) {
    std::string lower = device_name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower.find("nvidia") != std::string::npos ||
        lower.find("amdgpu") != std::string::npos ||
        lower.find("radeon") != std::string::npos ||
        lower.find("i915") != std::string::npos ||
        lower.find("gpu") != std::string::npos) {
        return DeviceClass::GPU;
    }

    if (lower.find("nvme") != std::string::npos) {
        return DeviceClass::NVMe;
    }

    if (lower.find("eth") != std::string::npos ||
        lower.find("eno") != std::string::npos ||
        lower.find("enp") != std::string::npos ||
        lower.find("wlp") != std::string::npos ||
        lower.find("mlx") != std::string::npos ||
        lower.find("ixgbe") != std::string::npos ||
        lower.find("i40e") != std::string::npos ||
        lower.find("igb") != std::string::npos) {
        return DeviceClass::NIC;
    }

    if (lower.find("usb") != std::string::npos ||
        lower.find("xhci") != std::string::npos ||
        lower.find("ehci") != std::string::npos ||
        lower.find("ohci") != std::string::npos) {
        return DeviceClass::USB;
    }

    if (lower.find("snd") != std::string::npos ||
        lower.find("hda") != std::string::npos ||
        lower.find("audio") != std::string::npos) {
        return DeviceClass::Audio;
    }

    if (lower.find("vpu") != std::string::npos) {
        return DeviceClass::VPU;
    }

    return DeviceClass::Other;
}

int AffinityOptimizer::get_device_numa_node(const std::string& device_name) {
    // Search PCI devices for one matching this device name
    std::string pci_base = "/sys/bus/pci/devices";
    if (!std::filesystem::exists(pci_base)) return 0;

    for (const auto& entry : std::filesystem::directory_iterator(pci_base)) {
        // Check if driver name matches
        auto driver_link = entry.path() / "driver";
        if (std::filesystem::is_symlink(driver_link)) {
            std::string driver = std::filesystem::read_symlink(driver_link).filename().string();
            if (device_name.find(driver) != std::string::npos) {
                std::ifstream numa_file(entry.path() / "numa_node");
                if (numa_file.is_open()) {
                    int node = -1;
                    numa_file >> node;
                    return node >= 0 ? node : 0;
                }
            }
        }

        // Check net interface name
        auto net_dir = entry.path() / "net";
        if (std::filesystem::exists(net_dir)) {
            for (const auto& net_entry : std::filesystem::directory_iterator(net_dir)) {
                if (net_entry.path().filename().string() == device_name) {
                    std::ifstream numa_file(entry.path() / "numa_node");
                    if (numa_file.is_open()) {
                        int node = -1;
                        numa_file >> node;
                        return node >= 0 ? node : 0;
                    }
                }
            }
        }
    }

    return 0;
}

std::vector<uint32_t> AffinityOptimizer::cpus_on_numa(
    const std::vector<CpuTopology>& topology, uint32_t numa_node) {
    std::vector<uint32_t> cpus;
    for (const auto& cpu : topology) {
        if (cpu.numa_node == numa_node) {
            cpus.push_back(cpu.cpu_id);
        }
    }
    return cpus;
}

uint32_t AffinityOptimizer::least_loaded_cpu(
    const std::vector<CpuTopology>& topology,
    const std::vector<uint32_t>& candidates) {
    if (candidates.empty()) return 0;

    uint32_t best = candidates[0];
    uint64_t best_load = UINT64_MAX;

    for (uint32_t cpu_id : candidates) {
        for (const auto& cpu : topology) {
            if (cpu.cpu_id == cpu_id && !cpu.is_hot) {
                if (cpu.interrupt_load < best_load) {
                    best_load = cpu.interrupt_load;
                    best = cpu_id;
                }
                break;
            }
        }
    }

    return best;
}

std::vector<uint32_t> AffinityOptimizer::least_loaded_cpus(
    const std::vector<CpuTopology>& topology,
    const std::vector<uint32_t>& candidates,
    uint32_t n) {
    // Sort candidates by load
    std::vector<std::pair<uint64_t, uint32_t>> sorted;
    for (uint32_t cpu_id : candidates) {
        for (const auto& cpu : topology) {
            if (cpu.cpu_id == cpu_id && !cpu.is_hot) {
                sorted.emplace_back(cpu.interrupt_load, cpu_id);
                break;
            }
        }
    }

    std::sort(sorted.begin(), sorted.end());

    std::vector<uint32_t> result;
    for (size_t i = 0; i < n && i < sorted.size(); ++i) {
        result.push_back(sorted[i].second);
    }

    return result;
}

std::vector<uint32_t> AffinityOptimizer::cool_cpus(
    const std::vector<CpuTopology>& topology,
    const std::vector<uint32_t>& candidates) {
    std::vector<uint32_t> result;
    for (uint32_t cpu_id : candidates) {
        for (const auto& cpu : topology) {
            if (cpu.cpu_id == cpu_id && !cpu.is_hot) {
                result.push_back(cpu_id);
                break;
            }
        }
    }
    // If all are hot, return all candidates anyway (better than nothing)
    return result.empty() ? candidates : result;
}

Result<std::vector<AffinityRecommendation>, std::string> AffinityOptimizer::recommend() {
    auto topology = build_topology();
    read_thermal_data(topology);
    read_interrupt_load(topology);

    auto irqs_result = IrqMapper::scan_irqs();
    if (!irqs_result.has_value()) {
        return Result<std::vector<AffinityRecommendation>, std::string>::error(
            irqs_result.error());
    }

    std::vector<AffinityRecommendation> recommendations;

    for (const auto& irq : irqs_result.value()) {
        AffinityRecommendation rec;
        rec.irq_number = irq.irq_number;
        rec.device_name = irq.device_name;
        rec.device_class = classify_device(irq.device_name);
        rec.previous_cpus = irq.current_cpu_affinity;

        int numa_node = get_device_numa_node(irq.device_name);
        auto numa_cpus = cpus_on_numa(topology, static_cast<uint32_t>(numa_node));
        if (numa_cpus.empty()) {
            // Fall back to all CPUs
            for (const auto& cpu : topology) {
                numa_cpus.push_back(cpu.cpu_id);
            }
        }

        auto eligible = cool_cpus(topology, numa_cpus);

        switch (rec.device_class) {
            case DeviceClass::GPU:
            case DeviceClass::VPU: {
                // GPU/VPU IRQs: pin to the single least-loaded NUMA-local core
                uint32_t best = least_loaded_cpu(topology, eligible);
                rec.recommended_cpus = {best};
                rec.reason = "GPU/VPU: pinned to least-loaded NUMA-local core " +
                           std::to_string(best) + " (node " + std::to_string(numa_node) + ")";
                break;
            }

            case DeviceClass::NVMe: {
                // NVMe: spread across NUMA-local cores (one per queue)
                // Estimate queue count from MSI-X vectors
                uint32_t queue_count = 4; // default
                if (irq.type == IrqType::MSIX) {
                    queue_count = std::min(static_cast<uint32_t>(eligible.size()),
                                          static_cast<uint32_t>(8));
                }
                rec.recommended_cpus = least_loaded_cpus(topology, eligible, queue_count);
                rec.reason = "NVMe: spread across " + std::to_string(rec.recommended_cpus.size()) +
                           " NUMA-local cores (node " + std::to_string(numa_node) + ")";
                break;
            }

            case DeviceClass::NIC: {
                // NIC: cores not used by latency-sensitive services
                // Avoid CPU 0 (often used for system tasks) and hot cores
                std::vector<uint32_t> nic_eligible;
                for (uint32_t cpu_id : eligible) {
                    if (cpu_id != 0) nic_eligible.push_back(cpu_id);
                }
                if (nic_eligible.empty()) nic_eligible = eligible;

                uint32_t queue_count = std::min(static_cast<uint32_t>(nic_eligible.size()),
                                               static_cast<uint32_t>(4));
                rec.recommended_cpus = least_loaded_cpus(topology, nic_eligible, queue_count);
                rec.reason = "NIC: " + std::to_string(rec.recommended_cpus.size()) +
                           " cores (avoiding CPU0, NUMA node " + std::to_string(numa_node) + ")";
                break;
            }

            case DeviceClass::USB:
            case DeviceClass::Audio: {
                // USB/Audio: any low-priority core, preferring least loaded
                uint32_t best = least_loaded_cpu(topology, eligible);
                rec.recommended_cpus = {best};
                rec.reason = "USB/Audio: low-priority core " + std::to_string(best);
                break;
            }

            default: {
                // Other: least-loaded NUMA-local core
                uint32_t best = least_loaded_cpu(topology, eligible);
                rec.recommended_cpus = {best};
                rec.reason = "Default: least-loaded NUMA-local core " + std::to_string(best);
                break;
            }
        }

        recommendations.push_back(std::move(rec));
    }

    return Result<std::vector<AffinityRecommendation>, std::string>::ok(
        std::move(recommendations));
}

Result<std::vector<AffinityRecommendation>, std::string> AffinityOptimizer::optimize() {
    auto recs_result = recommend();
    if (!recs_result.has_value()) {
        return recs_result;
    }

    auto& recs = recs_result.value();
    std::vector<AffinityRecommendation> applied;

    for (const auto& rec : recs) {
        if (rec.recommended_cpus.empty()) continue;

        // Skip if already optimal
        if (rec.recommended_cpus == rec.previous_cpus) continue;

        auto result = IrqMapper::set_affinity(rec.irq_number, rec.recommended_cpus);
        if (result.has_value()) {
            applied.push_back(rec);
        }
    }

    return Result<std::vector<AffinityRecommendation>, std::string>::ok(std::move(applied));
}

Result<std::vector<AffinityRecommendation>, std::string>
AffinityOptimizer::rebalance_on_thermal(uint32_t hot_core) {
    auto topology = build_topology();
    read_thermal_data(topology);
    read_interrupt_load(topology);

    // Mark the specified core as hot
    for (auto& cpu : topology) {
        if (cpu.cpu_id == hot_core) {
            cpu.is_hot = true;
        }
    }

    auto irqs_result = IrqMapper::scan_irqs();
    if (!irqs_result.has_value()) {
        return Result<std::vector<AffinityRecommendation>, std::string>::error(
            irqs_result.error());
    }

    std::vector<AffinityRecommendation> changes;

    for (const auto& irq : irqs_result.value()) {
        // Check if this IRQ is currently pinned to the hot core
        bool on_hot_core = false;
        for (uint32_t cpu : irq.current_cpu_affinity) {
            if (cpu == hot_core) {
                on_hot_core = true;
                break;
            }
        }
        if (!on_hot_core) continue;

        // Find an alternative core on the same NUMA node
        int numa_node = get_device_numa_node(irq.device_name);
        auto numa_cpus = cpus_on_numa(topology, static_cast<uint32_t>(numa_node));
        auto eligible = cool_cpus(topology, numa_cpus);

        if (eligible.empty()) continue;

        uint32_t best = least_loaded_cpu(topology, eligible);

        AffinityRecommendation rec;
        rec.irq_number = irq.irq_number;
        rec.device_name = irq.device_name;
        rec.device_class = classify_device(irq.device_name);
        rec.previous_cpus = irq.current_cpu_affinity;
        rec.recommended_cpus = {best};
        rec.reason = "Thermal migration from hot core " + std::to_string(hot_core) +
                   " to core " + std::to_string(best);

        auto result = IrqMapper::set_affinity(irq.irq_number, rec.recommended_cpus);
        if (result.has_value()) {
            changes.push_back(std::move(rec));
        }
    }

    return Result<std::vector<AffinityRecommendation>, std::string>::ok(std::move(changes));
}

std::string AffinityOptimizer::balance_report() {
    auto topology = build_topology();
    read_thermal_data(topology);
    read_interrupt_load(topology);

    std::ostringstream oss;
    oss << "Interrupt Balance Report\n";
    oss << "========================\n\n";

    // Per-CPU load
    uint64_t total_interrupts = 0;
    uint64_t max_load = 0;
    uint64_t min_load = UINT64_MAX;

    for (const auto& cpu : topology) {
        total_interrupts += cpu.interrupt_load;
        max_load = std::max(max_load, cpu.interrupt_load);
        min_load = std::min(min_load, cpu.interrupt_load);
    }

    double avg_load = topology.empty() ? 0.0 :
        static_cast<double>(total_interrupts) / static_cast<double>(topology.size());

    oss << "CPU Interrupt Distribution:\n";
    for (const auto& cpu : topology) {
        double pct = total_interrupts > 0
            ? static_cast<double>(cpu.interrupt_load) / static_cast<double>(total_interrupts) * 100.0
            : 0.0;

        char temp_str[32];
        std::snprintf(temp_str, sizeof(temp_str), "%.1f", cpu.temperature_celsius);

        oss << "  CPU " << cpu.cpu_id
            << " (NUMA " << cpu.numa_node << ")"
            << ": " << cpu.interrupt_load << " ints"
            << " (" << static_cast<int>(pct) << "%)"
            << " temp=" << temp_str << "C"
            << (cpu.is_hot ? " [HOT]" : "")
            << "\n";
    }

    oss << "\nSummary:\n";
    oss << "  Total interrupts: " << total_interrupts << "\n";
    oss << "  Average per CPU:  " << static_cast<uint64_t>(avg_load) << "\n";
    oss << "  Max CPU load:     " << max_load << "\n";
    oss << "  Min CPU load:     " << min_load << "\n";

    // Imbalance ratio
    if (min_load > 0 && topology.size() > 1) {
        double imbalance = static_cast<double>(max_load) / static_cast<double>(min_load);
        char ratio_str[32];
        std::snprintf(ratio_str, sizeof(ratio_str), "%.1f", imbalance);
        oss << "  Imbalance ratio:  " << ratio_str << "x\n";

        if (imbalance > 10.0) {
            oss << "  WARNING: Severe interrupt imbalance detected!\n";
        } else if (imbalance > 3.0) {
            oss << "  NOTE: Moderate interrupt imbalance — consider optimization.\n";
        } else {
            oss << "  Status: Well balanced.\n";
        }
    }

    // Hot core warnings
    std::vector<uint32_t> hot_cores;
    for (const auto& cpu : topology) {
        if (cpu.is_hot) hot_cores.push_back(cpu.cpu_id);
    }
    if (!hot_cores.empty()) {
        oss << "\nThermal Warnings:\n";
        for (uint32_t core : hot_cores) {
            oss << "  CPU " << core << " exceeds thermal threshold ("
                << THERMAL_THRESHOLD << "C)\n";
        }
    }

    return oss.str();
}

} // namespace straylight
