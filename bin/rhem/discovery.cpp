// bin/rhem/discovery.cpp
#include "discovery.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

namespace straylight::rhem {

namespace fs = std::filesystem;

std::string DeviceDiscovery::read_file_contents(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs) return "";
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

bool DeviceDiscovery::path_exists(const std::string& path) {
    std::error_code ec;
    return fs::exists(path, ec);
}

Device DeviceDiscovery::probe_cpu() {
    Device cpu;
    cpu.id = 0;
    cpu.type = DeviceType::CPU;
    cpu.available = true;

    // Try to get CPU model name from /proc/cpuinfo
    std::string cpuinfo = read_file_contents("/proc/cpuinfo");
    if (!cpuinfo.empty()) {
        std::istringstream iss(cpuinfo);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.find("model name") != std::string::npos) {
                auto pos = line.find(':');
                if (pos != std::string::npos && pos + 2 < line.size()) {
                    cpu.name = line.substr(pos + 2);
                }
                break;
            }
        }
    }
    if (cpu.name.empty()) {
        cpu.name = "CPU (" + std::to_string(std::thread::hardware_concurrency()) + " cores)";
    }

    // Try to get total memory from /proc/meminfo
    std::string meminfo = read_file_contents("/proc/meminfo");
    if (!meminfo.empty()) {
        std::istringstream iss(meminfo);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.find("MemTotal:") != std::string::npos) {
                // Format: "MemTotal:       12345678 kB"
                std::istringstream ls(line);
                std::string label;
                size_t kb = 0;
                ls >> label >> kb;
                cpu.memory_bytes = kb * 1024;
                break;
            }
        }
    }
    if (cpu.memory_bytes == 0) {
        // Fallback: estimate 8GB
        cpu.memory_bytes = 8ULL * 1024 * 1024 * 1024;
    }

    // Estimate CPU TFLOPS: cores * 2 GHz * 16 (AVX-512 FLOPS/cycle) / 1e3
    unsigned int cores = std::thread::hardware_concurrency();
    if (cores == 0) cores = 4;
    cpu.compute_tflops = static_cast<float>(cores) * 2.0f * 16.0f / 1000.0f;

    return cpu;
}

std::vector<Device> DeviceDiscovery::probe_nvidia(uint32_t& next_id) {
    std::vector<Device> devices;

    // Scan /dev/nvidia0, /dev/nvidia1, ...
    for (int i = 0; i < 16; ++i) {
        std::string dev_path = "/dev/nvidia" + std::to_string(i);
        if (!path_exists(dev_path)) break;

        Device dev;
        dev.id = next_id++;
        dev.type = DeviceType::CUDA;
        dev.available = true;

        // Try to read GPU name from /proc/driver/nvidia/gpus/<bus>/information
        std::string gpu_dir = "/proc/driver/nvidia/gpus/";
        std::error_code ec;
        if (fs::exists(gpu_dir, ec)) {
            int gpu_idx = 0;
            for (auto& entry : fs::directory_iterator(gpu_dir, ec)) {
                if (gpu_idx == i) {
                    std::string info_path = entry.path().string() + "/information";
                    std::string info = read_file_contents(info_path);
                    if (!info.empty()) {
                        std::istringstream iss(info);
                        std::string line;
                        while (std::getline(iss, line)) {
                            if (line.find("Model:") != std::string::npos) {
                                auto pos = line.find(':');
                                if (pos != std::string::npos && pos + 2 < line.size()) {
                                    dev.name = line.substr(pos + 2);
                                }
                                break;
                            }
                        }
                    }
                    break;
                }
                ++gpu_idx;
            }
        }

        if (dev.name.empty()) {
            dev.name = "NVIDIA GPU " + std::to_string(i);
        }

        // Default estimates if we can't read actual values
        dev.memory_bytes = 8ULL * 1024 * 1024 * 1024;  // 8GB estimate
        dev.compute_tflops = 10.0f;  // conservative estimate

        devices.push_back(std::move(dev));
    }

    return devices;
}

std::vector<Device> DeviceDiscovery::probe_rocm(uint32_t& next_id) {
    std::vector<Device> devices;

    // Scan /sys/class/drm/card* for AMD GPUs
    std::string drm_path = "/sys/class/drm";
    std::error_code ec;
    if (!fs::exists(drm_path, ec)) return devices;

    for (auto& entry : fs::directory_iterator(drm_path, ec)) {
        std::string name = entry.path().filename().string();
        // Look for card0, card1, etc. (skip renderD* nodes)
        if (name.find("card") != 0 || name.find("render") != std::string::npos) {
            continue;
        }

        // Check if this is an AMD device by reading vendor ID
        std::string vendor_path = entry.path().string() + "/device/vendor";
        std::string vendor = read_file_contents(vendor_path);
        // AMD vendor ID = 0x1002
        if (vendor.find("0x1002") == std::string::npos) continue;

        Device dev;
        dev.id = next_id++;
        dev.type = DeviceType::ROCm;
        dev.available = true;

        // Try to read device name
        std::string product_path = entry.path().string() + "/device/product_name";
        dev.name = read_file_contents(product_path);
        if (dev.name.empty()) {
            dev.name = "AMD GPU (" + name + ")";
        }
        // Trim trailing whitespace/newlines
        while (!dev.name.empty() &&
               (dev.name.back() == '\n' || dev.name.back() == '\r' || dev.name.back() == ' ')) {
            dev.name.pop_back();
        }

        // Read VRAM size from mem_info_vram_total if available
        std::string vram_path = entry.path().string() + "/device/mem_info_vram_total";
        std::string vram_str = read_file_contents(vram_path);
        if (!vram_str.empty()) {
            try {
                dev.memory_bytes = std::stoull(vram_str);
            } catch (...) {
                dev.memory_bytes = 8ULL * 1024 * 1024 * 1024;
            }
        } else {
            dev.memory_bytes = 8ULL * 1024 * 1024 * 1024;
        }

        dev.compute_tflops = 8.0f;  // conservative estimate
        devices.push_back(std::move(dev));
    }

    return devices;
}

std::vector<Device> DeviceDiscovery::probe_fpga(uint32_t& next_id) {
    std::vector<Device> devices;

    // Scan for Xilinx FPGA management devices
    for (int i = 0; i < 8; ++i) {
        std::string dev_path = "/dev/xclmgmt" + std::to_string(i);
        if (!path_exists(dev_path)) {
            // Also check /sys/class/fpga_manager
            std::string fpga_path = "/sys/class/fpga_manager/fpga" + std::to_string(i);
            if (!path_exists(fpga_path)) continue;
        }

        Device dev;
        dev.id = next_id++;
        dev.type = DeviceType::FPGA;
        dev.name = "FPGA Device " + std::to_string(i);
        dev.memory_bytes = 4ULL * 1024 * 1024 * 1024;  // 4GB typical
        dev.compute_tflops = 1.0f;
        dev.available = true;
        devices.push_back(std::move(dev));
    }

    return devices;
}

std::vector<Device> DeviceDiscovery::probe_tpu(uint32_t& next_id) {
    std::vector<Device> devices;

    // Scan /dev/accel* for TPU accelerators
    for (int i = 0; i < 8; ++i) {
        std::string dev_path = "/dev/accel" + std::to_string(i);
        if (!path_exists(dev_path)) continue;

        Device dev;
        dev.id = next_id++;
        dev.type = DeviceType::TPU;
        dev.name = "TPU Device " + std::to_string(i);
        dev.memory_bytes = 16ULL * 1024 * 1024 * 1024;  // 16GB typical
        dev.compute_tflops = 30.0f;
        dev.available = true;
        devices.push_back(std::move(dev));
    }

    return devices;
}

straylight::Result<std::vector<Device>, std::string> DeviceDiscovery::scan() {
    std::vector<Device> devices;

    // CPU is always present
    devices.push_back(probe_cpu());

    uint32_t next_id = 1;

    // Probe accelerators
    auto nvidia_devs = probe_nvidia(next_id);
    devices.insert(devices.end(), nvidia_devs.begin(), nvidia_devs.end());

    auto rocm_devs = probe_rocm(next_id);
    devices.insert(devices.end(), rocm_devs.begin(), rocm_devs.end());

    auto fpga_devs = probe_fpga(next_id);
    devices.insert(devices.end(), fpga_devs.begin(), fpga_devs.end());

    auto tpu_devs = probe_tpu(next_id);
    devices.insert(devices.end(), tpu_devs.begin(), tpu_devs.end());

    return straylight::Result<std::vector<Device>, std::string>::ok(std::move(devices));
}

} // namespace straylight::rhem
