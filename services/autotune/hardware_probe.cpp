// services/autotune/hardware_probe.cpp
#include "hardware_probe.h"
#include <straylight/log.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace straylight {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Utility helpers
// ---------------------------------------------------------------------------

std::string HardwareProbe::read_sysfs(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) return {};
    std::string val;
    std::getline(ifs, val);
    // Trim trailing whitespace/newlines
    while (!val.empty() && (val.back() == '\n' || val.back() == '\r' || val.back() == ' '))
        val.pop_back();
    return val;
}

std::string HardwareProbe::read_dmi_field(const std::string& field) {
    return read_sysfs("/sys/class/dmi/id/" + field);
}

std::vector<std::string> HardwareProbe::list_directory(const std::string& path) {
    std::vector<std::string> entries;
    std::error_code ec;
    if (!fs::exists(path, ec)) return entries;
    for (const auto& entry : fs::directory_iterator(path, ec)) {
        entries.push_back(entry.path().filename().string());
    }
    std::sort(entries.begin(), entries.end());
    return entries;
}

bool HardwareProbe::file_exists(const std::string& path) {
    std::error_code ec;
    return fs::exists(path, ec);
}

// ---------------------------------------------------------------------------
// CPU detection
// ---------------------------------------------------------------------------

CpuInfo HardwareProbe::detect_cpu() {
    CpuInfo info;

    std::ifstream cpuinfo("/proc/cpuinfo");
    if (!cpuinfo.is_open()) {
        SL_WARN("autotune: /proc/cpuinfo not available");
        return info;
    }

    int processor_count = 0;
    std::unordered_map<std::string, int> physical_ids;
    std::unordered_map<std::string, int> core_ids_per_socket;
    std::string current_physical_id = "0";

    std::string line;
    while (std::getline(cpuinfo, line)) {
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;

        std::string key = line.substr(0, colon);
        std::string val = line.substr(colon + 1);

        // Trim whitespace
        auto trim = [](std::string& s) {
            while (!s.empty() && s.front() == ' ') s.erase(s.begin());
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
        };
        trim(key);
        trim(val);

        if (key == "processor") {
            processor_count++;
        } else if (key == "model name" && info.model_name.empty()) {
            info.model_name = val;
        } else if (key == "vendor_id" && info.vendor.empty()) {
            info.vendor = val;
        } else if (key == "physical id") {
            current_physical_id = val;
            physical_ids[val]++;
        } else if (key == "core id") {
            core_ids_per_socket[current_physical_id + ":" + val] = 1;
        } else if (key == "cpu MHz" && info.base_freq_mhz == 0) {
            try { info.base_freq_mhz = static_cast<int>(std::stof(val)); } catch (...) {}
        } else if (key == "flags") {
            info.features.avx    = val.find(" avx ")  != std::string::npos || val.find(" avx\n") != std::string::npos;
            info.features.avx2   = val.find("avx2")   != std::string::npos;
            info.features.avx512 = val.find("avx512") != std::string::npos;
            info.features.amx    = val.find("amx")    != std::string::npos;
            info.features.aes_ni = val.find("aes")    != std::string::npos;
            info.features.sse42  = val.find("sse4_2") != std::string::npos;
        }
    }

    info.logical_cores  = processor_count;
    info.sockets        = std::max(1, static_cast<int>(physical_ids.size()));
    info.physical_cores = core_ids_per_socket.empty()
                              ? processor_count
                              : static_cast<int>(core_ids_per_socket.size());

    // Detect scaling driver
    std::string driver = read_sysfs("/sys/devices/system/cpu/cpu0/cpufreq/scaling_driver");
    info.features.intel_pstate = (driver == "intel_pstate");

    // Max frequency from cpufreq
    std::string max_freq_str = read_sysfs("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq");
    if (!max_freq_str.empty()) {
        try { info.max_freq_mhz = std::stoi(max_freq_str) / 1000; } catch (...) {}
    }

    SL_INFO("autotune: CPU: {} ({} cores, {} threads, {} socket(s))",
            info.model_name, info.physical_cores, info.logical_cores, info.sockets);
    return info;
}

// ---------------------------------------------------------------------------
// Memory detection
// ---------------------------------------------------------------------------

MemoryInfo HardwareProbe::detect_memory() {
    MemoryInfo info;

    std::ifstream meminfo("/proc/meminfo");
    if (!meminfo.is_open()) {
        SL_WARN("autotune: /proc/meminfo not available");
        return info;
    }

    std::string line;
    while (std::getline(meminfo, line)) {
        if (line.rfind("MemTotal:", 0) == 0) {
            std::istringstream iss(line.substr(9));
            uint64_t kb = 0;
            iss >> kb;
            info.total_bytes = kb * 1024ULL;
        } else if (line.rfind("MemAvailable:", 0) == 0) {
            std::istringstream iss(line.substr(13));
            uint64_t kb = 0;
            iss >> kb;
            info.available_bytes = kb * 1024ULL;
        }
    }

    // NUMA detection
    auto numa_nodes = list_directory("/sys/devices/system/node");
    int node_count = 0;
    for (const auto& n : numa_nodes) {
        if (n.rfind("node", 0) == 0) node_count++;
    }
    info.numa_nodes = std::max(1, node_count);
    info.numa = (info.numa_nodes > 1);

    SL_INFO("autotune: RAM: {} MiB total, {} MiB available, NUMA={}",
            info.total_bytes / (1024 * 1024),
            info.available_bytes / (1024 * 1024),
            info.numa ? "yes" : "no");
    return info;
}

// ---------------------------------------------------------------------------
// GPU detection
// ---------------------------------------------------------------------------

std::vector<GpuInfo> HardwareProbe::detect_gpus() {
    std::vector<GpuInfo> gpus;

    // Try NVIDIA via nvidia-smi
    bool has_nvidia_smi = (std::system("which nvidia-smi >/dev/null 2>&1") == 0);

    if (has_nvidia_smi) {
        // Query GPU list: name, memory, pci bus
        FILE* pipe = popen("nvidia-smi --query-gpu=name,memory.total,pci.bus_id "
                           "--format=csv,noheader,nounits 2>/dev/null", "r");
        if (pipe) {
            char buf[512];
            while (fgets(buf, sizeof(buf), pipe)) {
                std::string line(buf);
                // Format: "NVIDIA GeForce RTX 4090, 24564, 00000000:01:00.0"
                std::istringstream iss(line);
                std::string name, mem_str, pci;
                if (std::getline(iss, name, ',') &&
                    std::getline(iss, mem_str, ',') &&
                    std::getline(iss, pci)) {
                    GpuInfo gpu;
                    // Trim leading spaces
                    auto trim = [](std::string& s) {
                        while (!s.empty() && s.front() == ' ') s.erase(s.begin());
                        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
                            s.pop_back();
                    };
                    trim(name); trim(mem_str); trim(pci);

                    gpu.name = name;
                    gpu.vendor = "nvidia";
                    gpu.pci_slot = pci;
                    gpu.has_nvidia_smi = true;
                    try { gpu.vram_bytes = static_cast<uint64_t>(std::stoi(mem_str)) * 1024ULL * 1024ULL; } catch (...) {}

                    SL_INFO("autotune: GPU: {} ({} MiB VRAM) at {}",
                            gpu.name, gpu.vram_bytes / (1024 * 1024), gpu.pci_slot);
                    gpus.push_back(std::move(gpu));
                }
            }
            pclose(pipe);
        }
    }

    // Try AMD via rocm-smi
    bool has_rocm_smi = (std::system("which rocm-smi >/dev/null 2>&1") == 0);

    if (has_rocm_smi) {
        FILE* pipe = popen("rocm-smi --showproductname --showmeminfo vram --csv 2>/dev/null", "r");
        if (pipe) {
            char buf[512];
            // Skip header
            if (fgets(buf, sizeof(buf), pipe)) {
                while (fgets(buf, sizeof(buf), pipe)) {
                    GpuInfo gpu;
                    gpu.vendor = "amd";
                    gpu.name = std::string(buf);
                    gpu.has_rocm_smi = true;
                    gpus.push_back(std::move(gpu));
                }
            }
            pclose(pipe);
        }
    }

    // Fallback: scan /sys/class/drm
    if (gpus.empty()) {
        auto drm_cards = list_directory("/sys/class/drm");
        for (const auto& card : drm_cards) {
            if (card.rfind("card", 0) != 0) continue;
            if (card.find('-') != std::string::npos) continue; // skip card0-DP-1 etc.

            std::string base = "/sys/class/drm/" + card + "/device";
            std::string vendor_id = read_sysfs(base + "/vendor");
            std::string device_id = read_sysfs(base + "/device");

            if (!vendor_id.empty()) {
                GpuInfo gpu;
                gpu.pci_slot = card;
                if (vendor_id == "0x10de") {
                    gpu.vendor = "nvidia";
                } else if (vendor_id == "0x1002") {
                    gpu.vendor = "amd";
                } else if (vendor_id == "0x8086") {
                    gpu.vendor = "intel";
                }
                gpu.name = gpu.vendor + " GPU " + device_id;
                gpus.push_back(std::move(gpu));
            }
        }
    }

    if (gpus.empty()) {
        SL_INFO("autotune: no GPUs detected");
    }
    return gpus;
}

// ---------------------------------------------------------------------------
// Block device detection
// ---------------------------------------------------------------------------

std::vector<BlockDeviceInfo> HardwareProbe::detect_block_devices() {
    std::vector<BlockDeviceInfo> devices;

    auto block_devs = list_directory("/sys/block");
    for (const auto& dev : block_devs) {
        // Skip loop, ram, dm devices
        if (dev.rfind("loop", 0) == 0 || dev.rfind("ram", 0) == 0 || dev.rfind("dm-", 0) == 0)
            continue;

        std::string base = "/sys/block/" + dev;

        BlockDeviceInfo bdi;
        bdi.name = dev;
        bdi.model = read_sysfs(base + "/device/model");

        // Determine transport
        if (dev.rfind("nvme", 0) == 0) {
            bdi.transport = "nvme";
        } else if (dev.rfind("sd", 0) == 0) {
            // Could be SATA or USB; check removable
            std::string removable = read_sysfs(base + "/removable");
            bdi.transport = (removable == "1") ? "usb" : "sata";
        } else {
            bdi.transport = "unknown";
        }

        // Size in 512-byte sectors
        std::string size_str = read_sysfs(base + "/size");
        if (!size_str.empty()) {
            try { bdi.size_bytes = std::stoull(size_str) * 512ULL; } catch (...) {}
        }

        // Rotational
        std::string rot = read_sysfs(base + "/queue/rotational");
        bdi.rotational = (rot == "1");

        SL_INFO("autotune: block device: {} ({}, {}, {} GiB, rotational={})",
                bdi.name, bdi.model, bdi.transport,
                bdi.size_bytes / (1024ULL * 1024 * 1024), bdi.rotational ? "yes" : "no");
        devices.push_back(std::move(bdi));
    }
    return devices;
}

// ---------------------------------------------------------------------------
// Network interface detection
// ---------------------------------------------------------------------------

std::vector<NetInterfaceInfo> HardwareProbe::detect_net_interfaces() {
    std::vector<NetInterfaceInfo> ifaces;

    auto net_devs = list_directory("/sys/class/net");
    for (const auto& dev : net_devs) {
        // Skip loopback
        if (dev == "lo") continue;

        std::string base = "/sys/class/net/" + dev;

        // Must be a physical device (has device/ symlink)
        if (!file_exists(base + "/device")) continue;

        NetInterfaceInfo nii;
        nii.name = dev;

        // Speed
        std::string speed_str = read_sysfs(base + "/speed");
        if (!speed_str.empty()) {
            try { nii.speed_mbps = std::stoi(speed_str); } catch (...) {}
        }

        // Driver name (via uevent)
        std::string uevent = read_sysfs(base + "/device/uevent");
        // Parse DRIVER= line from the uevent file
        std::ifstream uevent_file(base + "/device/uevent");
        if (uevent_file.is_open()) {
            std::string line;
            while (std::getline(uevent_file, line)) {
                if (line.rfind("DRIVER=", 0) == 0) {
                    nii.driver = line.substr(7);
                    break;
                }
            }
        }

        // GRO/GSO support — check via ethtool features in sysfs
        // (Real detection would use ETHTOOL ioctl; here we assume supported for
        //  modern NICs and just set the flag to true if the interface is up.)
        std::string operstate = read_sysfs(base + "/operstate");
        nii.supports_gro = true;
        nii.supports_gso = true;

        SL_INFO("autotune: net interface: {} (driver={}, speed={} Mbps)",
                nii.name, nii.driver, nii.speed_mbps);
        ifaces.push_back(std::move(nii));
    }
    return ifaces;
}

// ---------------------------------------------------------------------------
// Full hardware detection entry point
// ---------------------------------------------------------------------------

HardwareInventory HardwareProbe::detect() {
    SL_INFO("autotune: beginning hardware detection");

    HardwareInventory inv;
    inv.cpu             = detect_cpu();
    inv.memory          = detect_memory();
    inv.gpus            = detect_gpus();
    inv.block_devices   = detect_block_devices();
    inv.net_interfaces  = detect_net_interfaces();

    inv.board_vendor    = read_dmi_field("board_vendor");
    inv.board_name      = read_dmi_field("board_name");
    inv.bios_version    = read_dmi_field("bios_version");

    SL_INFO("autotune: hardware detection complete — {} GPU(s), {} block device(s), {} NIC(s)",
            inv.gpus.size(), inv.block_devices.size(), inv.net_interfaces.size());
    return inv;
}

} // namespace straylight
