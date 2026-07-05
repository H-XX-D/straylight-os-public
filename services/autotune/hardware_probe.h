// services/autotune/hardware_probe.h
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace straylight {

/// CPU feature flags detected at runtime.
struct CpuFeatures {
    bool avx       = false;
    bool avx2      = false;
    bool avx512    = false;
    bool amx       = false;
    bool aes_ni    = false;
    bool sse42     = false;
    bool intel_pstate = false;  // vs acpi-cpufreq
};

/// Summary of detected CPU hardware.
struct CpuInfo {
    std::string model_name;
    std::string vendor;           // "GenuineIntel", "AuthenticAMD"
    int physical_cores  = 0;
    int logical_cores   = 0;
    int sockets         = 1;
    int base_freq_mhz   = 0;
    int max_freq_mhz    = 0;
    CpuFeatures features;
};

/// Summary of detected memory hardware.
struct MemoryInfo {
    uint64_t total_bytes        = 0;
    uint64_t available_bytes    = 0;
    int speed_mhz               = 0;
    int channels                = 0;
    int dimm_count              = 0;
    bool numa                   = false;
    int numa_nodes              = 1;
};

/// Summary of a detected GPU.
struct GpuInfo {
    std::string name;
    std::string vendor;           // "nvidia", "amd", "intel"
    std::string pci_slot;
    uint64_t vram_bytes          = 0;
    int compute_capability_major = 0;
    int compute_capability_minor = 0;
    bool has_nvidia_smi          = false;
    bool has_rocm_smi            = false;
};

/// Summary of a block device.
struct BlockDeviceInfo {
    std::string name;             // "nvme0n1", "sda"
    std::string model;
    std::string transport;        // "nvme", "sata", "usb"
    uint64_t size_bytes          = 0;
    bool rotational              = false;
};

/// Summary of a network interface.
struct NetInterfaceInfo {
    std::string name;             // "eth0", "enp3s0"
    std::string driver;
    int speed_mbps               = 0;
    bool supports_gro            = false;
    bool supports_gso            = false;
    int rx_ring_max              = 0;
    int tx_ring_max              = 0;
};

/// Complete hardware inventory.
struct HardwareInventory {
    CpuInfo cpu;
    MemoryInfo memory;
    std::vector<GpuInfo> gpus;
    std::vector<BlockDeviceInfo> block_devices;
    std::vector<NetInterfaceInfo> net_interfaces;
    std::string board_vendor;
    std::string board_name;
    std::string bios_version;
};

/// Probes the current hardware and returns a complete inventory.
class HardwareProbe {
public:
    /// Run full hardware detection. Returns an inventory of all detected hardware.
    HardwareInventory detect();

private:
    CpuInfo detect_cpu();
    MemoryInfo detect_memory();
    std::vector<GpuInfo> detect_gpus();
    std::vector<BlockDeviceInfo> detect_block_devices();
    std::vector<NetInterfaceInfo> detect_net_interfaces();
    std::string read_dmi_field(const std::string& field);
    std::string read_sysfs(const std::string& path);
    std::vector<std::string> list_directory(const std::string& path);
    bool file_exists(const std::string& path);
};

} // namespace straylight
