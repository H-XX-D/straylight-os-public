// bin/rhem/discovery.h
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <straylight/result.h>

namespace straylight::rhem {

enum class DeviceType { CPU, CUDA, ROCm, FPGA, TPU };

struct Device {
    uint32_t id;
    DeviceType type;
    std::string name;
    size_t memory_bytes;
    float compute_tflops;
    bool available;
};

/// Scan the system for available compute devices.
class DeviceDiscovery {
public:
    /// Scan /sys/class/drm, /dev/nvidia*, /dev/accel*, /proc/cpuinfo etc.
    /// Returns all discovered devices (CPU always included).
    straylight::Result<std::vector<Device>, std::string> scan();

private:
    /// Probe CPU information from /proc/cpuinfo and /proc/meminfo.
    Device probe_cpu();

    /// Scan for NVIDIA GPUs via /dev/nvidia* and /proc/driver/nvidia/gpus/.
    std::vector<Device> probe_nvidia(uint32_t& next_id);

    /// Scan for AMD ROCm GPUs via /sys/class/drm/card*/device.
    std::vector<Device> probe_rocm(uint32_t& next_id);

    /// Scan for FPGAs via /dev/xclmgmt* or /sys/class/fpga_manager.
    std::vector<Device> probe_fpga(uint32_t& next_id);

    /// Scan for TPUs via /dev/accel*.
    std::vector<Device> probe_tpu(uint32_t& next_id);

    /// Read a file's contents as a string (utility).
    static std::string read_file_contents(const std::string& path);

    /// Check if a path exists.
    static bool path_exists(const std::string& path);
};

/// Convert DeviceType to string.
inline std::string device_type_str(DeviceType t) {
    switch (t) {
    case DeviceType::CPU:  return "CPU";
    case DeviceType::CUDA: return "CUDA";
    case DeviceType::ROCm: return "ROCm";
    case DeviceType::FPGA: return "FPGA";
    case DeviceType::TPU:  return "TPU";
    }
    return "Unknown";
}

/// Parse DeviceType from string.
inline DeviceType device_type_from_str(const std::string& s) {
    if (s == "CUDA" || s == "cuda") return DeviceType::CUDA;
    if (s == "ROCm" || s == "rocm") return DeviceType::ROCm;
    if (s == "FPGA" || s == "fpga") return DeviceType::FPGA;
    if (s == "TPU"  || s == "tpu")  return DeviceType::TPU;
    return DeviceType::CPU;
}

} // namespace straylight::rhem
