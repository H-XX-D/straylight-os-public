// services/nerve/irq_mapper.cpp
#include "irq_mapper.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <unistd.h>

namespace straylight {

std::string IrqMapper::cpus_to_mask(const std::vector<uint32_t>& cpu_ids) {
    if (cpu_ids.empty()) return "0";

    uint32_t max_cpu = *std::max_element(cpu_ids.begin(), cpu_ids.end());
    // Number of 32-bit words needed
    uint32_t words = (max_cpu / 32) + 1;
    std::vector<uint32_t> mask(words, 0);

    for (uint32_t cpu : cpu_ids) {
        mask[cpu / 32] |= (1u << (cpu % 32));
    }

    // Format as comma-separated 8-hex-digit groups (high word first)
    std::ostringstream oss;
    for (int i = static_cast<int>(words) - 1; i >= 0; --i) {
        if (i < static_cast<int>(words) - 1) oss << ",";
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%08x", mask[static_cast<size_t>(i)]);
        oss << buf;
    }

    // Strip leading zeros (but keep at least one character)
    std::string result = oss.str();
    size_t first_nonzero = result.find_first_not_of("0,");
    if (first_nonzero != std::string::npos && first_nonzero > 0) {
        result = result.substr(first_nonzero);
    }

    return result;
}

std::vector<uint32_t> IrqMapper::mask_to_cpus(const std::string& mask) {
    std::vector<uint32_t> cpus;

    // Remove commas and whitespace
    std::string clean;
    for (char c : mask) {
        if (std::isxdigit(c)) clean += c;
    }

    // Parse from right to left (LSB first)
    for (size_t i = 0; i < clean.size(); ++i) {
        char c = clean[clean.size() - 1 - i];
        uint32_t nibble = 0;
        if (c >= '0' && c <= '9') nibble = static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') nibble = static_cast<uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') nibble = static_cast<uint32_t>(c - 'A' + 10);

        for (uint32_t bit = 0; bit < 4; ++bit) {
            if (nibble & (1u << bit)) {
                cpus.push_back(static_cast<uint32_t>(i * 4 + bit));
            }
        }
    }

    std::sort(cpus.begin(), cpus.end());
    return cpus;
}

IrqType IrqMapper::detect_irq_type(uint32_t irq) {
    // Check /proc/irq/N/ for MSI indicators
    std::string irq_dir = "/proc/irq/" + std::to_string(irq);

    // Read the chip_name or type file if available
    std::ifstream chip_file(irq_dir + "/chip_name");
    if (chip_file.is_open()) {
        std::string chip;
        std::getline(chip_file, chip);
        if (chip.find("MSI-X") != std::string::npos) return IrqType::MSIX;
        if (chip.find("MSI") != std::string::npos) return IrqType::MSI;
    }

    // Check via /proc/irq/N/*/type or the actions
    for (const auto& entry : std::filesystem::directory_iterator(irq_dir)) {
        if (entry.is_directory()) {
            std::ifstream type_file(entry.path() / "type");
            if (type_file.is_open()) {
                std::string type;
                std::getline(type_file, type);
                if (type.find("MSI-X") != std::string::npos) return IrqType::MSIX;
                if (type.find("MSI") != std::string::npos) return IrqType::MSI;
            }
        }
    }

    // Heuristic: high IRQ numbers (>= 24) are typically MSI on modern systems
    if (irq >= 24) return IrqType::MSI;

    return IrqType::Legacy;
}

Result<IrqInfo, std::string> IrqMapper::parse_irq_line(const std::string& line,
                                                         uint32_t num_cpus) {
    std::istringstream iss(line);

    // First token is IRQ number (possibly followed by ':')
    std::string irq_str;
    iss >> irq_str;

    // Remove trailing ':'
    if (!irq_str.empty() && irq_str.back() == ':') {
        irq_str.pop_back();
    }

    // Skip non-numeric IRQs (NMI, LOC, etc.)
    bool is_numeric = true;
    for (char c : irq_str) {
        if (!std::isdigit(c)) { is_numeric = false; break; }
    }
    if (!is_numeric || irq_str.empty()) {
        return Result<IrqInfo, std::string>::error("Non-numeric IRQ: " + irq_str);
    }

    IrqInfo info;
    info.irq_number = static_cast<uint32_t>(std::stoul(irq_str));
    info.total_count = 0;

    // Read per-CPU counts
    for (uint32_t i = 0; i < num_cpus; ++i) {
        uint64_t count = 0;
        if (iss >> count) {
            info.total_count += count;
        }
    }

    // Remaining tokens: interrupt controller type + device name
    std::string token;
    std::vector<std::string> remaining;
    while (iss >> token) {
        remaining.push_back(token);
    }

    // Last token(s) are typically the device name
    if (!remaining.empty()) {
        // Skip the controller type (e.g., "IR-PCI-MSI-edge", "IO-APIC-edge")
        // Device name is usually the last entry
        info.device_name = remaining.back();

        // If there are multiple remaining, join the last few as device name
        // Controller types contain known patterns
        for (size_t i = 0; i < remaining.size(); ++i) {
            const auto& tok = remaining[i];
            if (tok.find("PCI") != std::string::npos ||
                tok.find("APIC") != std::string::npos ||
                tok.find("IR-") == 0 ||
                tok.find("IO-") == 0 ||
                tok.find("edge") != std::string::npos ||
                tok.find("fasteoi") != std::string::npos) {
                continue;
            }
            // Everything from here is device name
            info.device_name.clear();
            for (size_t j = i; j < remaining.size(); ++j) {
                if (!info.device_name.empty()) info.device_name += " ";
                info.device_name += remaining[j];
            }
            break;
        }
    }

    // Detect type and read current affinity
    info.type = detect_irq_type(info.irq_number);

    auto affinity = get_affinity(info.irq_number);
    if (affinity.has_value()) {
        info.current_cpu_affinity = mask_to_cpus(affinity.value());
    }

    return Result<IrqInfo, std::string>::ok(std::move(info));
}

Result<std::vector<IrqInfo>, std::string> IrqMapper::scan_irqs() {
    std::ifstream proc_ints("/proc/interrupts");
    if (!proc_ints.is_open()) {
        return Result<std::vector<IrqInfo>, std::string>::error(
            "Cannot open /proc/interrupts");
    }

    // First line is the CPU header
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

    if (num_cpus == 0) {
        num_cpus = static_cast<uint32_t>(::sysconf(_SC_NPROCESSORS_ONLN));
    }

    std::vector<IrqInfo> irqs;
    std::string line;
    while (std::getline(proc_ints, line)) {
        auto result = parse_irq_line(line, num_cpus);
        if (result.has_value()) {
            irqs.push_back(std::move(result).value());
        }
    }

    return Result<std::vector<IrqInfo>, std::string>::ok(std::move(irqs));
}

Result<std::string, std::string> IrqMapper::get_device_for_irq(uint32_t irq) {
    // Method 1: Check /proc/irq/N/ for action handlers
    std::string irq_dir = "/proc/irq/" + std::to_string(irq);
    if (std::filesystem::exists(irq_dir)) {
        for (const auto& entry : std::filesystem::directory_iterator(irq_dir)) {
            if (entry.is_directory() && entry.path().filename().string() != "." &&
                entry.path().filename().string() != "..") {
                return Result<std::string, std::string>::ok(
                    entry.path().filename().string());
            }
        }
    }

    // Method 2: Scan PCI devices for matching IRQ
    std::string pci_base = "/sys/bus/pci/devices";
    if (std::filesystem::exists(pci_base)) {
        for (const auto& entry : std::filesystem::directory_iterator(pci_base)) {
            std::ifstream irq_file(entry.path() / "irq");
            if (irq_file.is_open()) {
                uint32_t dev_irq = 0;
                irq_file >> dev_irq;
                if (dev_irq == irq) {
                    // Try to read the driver name
                    auto driver_link = entry.path() / "driver";
                    if (std::filesystem::is_symlink(driver_link)) {
                        return Result<std::string, std::string>::ok(
                            std::filesystem::read_symlink(driver_link).filename().string());
                    }
                    return Result<std::string, std::string>::ok(
                        entry.path().filename().string());
                }
            }
        }
    }

    // Method 3: Parse from /proc/interrupts
    auto irqs = scan_irqs();
    if (irqs.has_value()) {
        for (const auto& info : irqs.value()) {
            if (info.irq_number == irq) {
                return Result<std::string, std::string>::ok(info.device_name);
            }
        }
    }

    return Result<std::string, std::string>::error(
        "Cannot determine device for IRQ " + std::to_string(irq));
}

Result<void, std::string> IrqMapper::set_affinity(uint32_t irq, const std::string& cpu_mask) {
    std::string path = "/proc/irq/" + std::to_string(irq) + "/smp_affinity";

    std::ofstream ofs(path);
    if (!ofs.is_open()) {
        return Result<void, std::string>::error(
            "Cannot write to " + path + " — permission denied or IRQ does not exist");
    }

    ofs << cpu_mask;
    if (ofs.fail()) {
        return Result<void, std::string>::error(
            "Write failed to " + path + " — invalid mask or managed IRQ");
    }

    return Result<void, std::string>::ok();
}

Result<void, std::string> IrqMapper::set_affinity(uint32_t irq,
                                                    const std::vector<uint32_t>& cpu_ids) {
    if (cpu_ids.empty()) {
        return Result<void, std::string>::error("Empty CPU list");
    }
    return set_affinity(irq, cpus_to_mask(cpu_ids));
}

Result<std::string, std::string> IrqMapper::get_affinity(uint32_t irq) {
    std::string path = "/proc/irq/" + std::to_string(irq) + "/smp_affinity";

    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        return Result<std::string, std::string>::error(
            "Cannot read " + path);
    }

    std::string mask;
    std::getline(ifs, mask);

    // Trim whitespace
    while (!mask.empty() && (mask.back() == '\n' || mask.back() == ' ')) {
        mask.pop_back();
    }

    return Result<std::string, std::string>::ok(std::move(mask));
}

} // namespace straylight
