// services/nerve/irq_mapper.h
// Interrupt routing — discover, map, and set CPU affinity for hardware IRQs.
#pragma once

#include <straylight/result.h>

#include <cstdint>
#include <string>
#include <vector>

namespace straylight {

/// Type of interrupt mechanism.
enum class IrqType {
    Legacy,   // Traditional PIC/IOAPIC pin-based
    MSI,      // Message Signaled Interrupts
    MSIX      // MSI-X (extended, per-queue)
};

/// Information about a single IRQ line.
struct IrqInfo {
    uint32_t irq_number;
    std::string device_name;
    IrqType type;
    std::vector<uint32_t> current_cpu_affinity;  // Bitmask as list of CPU IDs
    uint64_t total_count;                         // Total interrupts observed
};

/// Reads and manipulates interrupt routing via procfs/sysfs.
class IrqMapper {
public:
    /// Scan /proc/interrupts to discover all IRQs and their current affinities.
    static Result<std::vector<IrqInfo>, std::string> scan_irqs();

    /// Map an IRQ number to the device that generates it.
    static Result<std::string, std::string> get_device_for_irq(uint32_t irq);

    /// Set CPU affinity for an IRQ — writes to /proc/irq/N/smp_affinity.
    /// cpu_mask is a hex bitmask string (e.g., "f" for CPUs 0-3).
    static Result<void, std::string> set_affinity(uint32_t irq, const std::string& cpu_mask);

    /// Set CPU affinity using a list of CPU IDs.
    static Result<void, std::string> set_affinity(uint32_t irq,
                                                   const std::vector<uint32_t>& cpu_ids);

    /// Get the current affinity mask for an IRQ.
    static Result<std::string, std::string> get_affinity(uint32_t irq);

    /// Convert a list of CPU IDs to a hex bitmask string.
    static std::string cpus_to_mask(const std::vector<uint32_t>& cpu_ids);

    /// Convert a hex bitmask string to a list of CPU IDs.
    static std::vector<uint32_t> mask_to_cpus(const std::string& mask);

private:
    /// Parse a single line from /proc/interrupts.
    static Result<IrqInfo, std::string> parse_irq_line(const std::string& line,
                                                        uint32_t num_cpus);

    /// Detect the IRQ type for a given IRQ number.
    static IrqType detect_irq_type(uint32_t irq);
};

} // namespace straylight
