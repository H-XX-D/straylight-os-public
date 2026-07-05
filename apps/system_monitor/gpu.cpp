// apps/system_monitor/gpu.cpp
// GPU monitoring via sysfs DRM nodes and nvidia-smi fallback
#include "gpu.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace straylight::sysmon {

namespace fs = std::filesystem;

GpuMonitor::GpuMonitor() = default;

void GpuMonitor::detect() {
    if (detected_) return;
    detected_ = true;

    // Scan /sys/class/drm/card* for GPU devices
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator("/sys/class/drm", ec)) {
        std::string name = entry.path().filename().string();

        // Only match "card0", "card1", etc. (not "card0-HDMI-A-1")
        if (name.compare(0, 4, "card") != 0) continue;
        if (name.find('-') != std::string::npos) continue;

        GpuInfo gpu;
        gpu.index = static_cast<int>(gpus_.size());

        // Read device info
        fs::path device_path = entry.path() / "device";

        // Vendor
        std::ifstream vendor_file(device_path / "vendor");
        std::string vendor_id;
        if (vendor_file.is_open()) {
            std::getline(vendor_file, vendor_id);
        }

        // Try reading the driver link
        fs::path driver_link = device_path / "driver";
        if (fs::is_symlink(driver_link, ec)) {
            auto target = fs::read_symlink(driver_link, ec);
            if (!ec) {
                gpu.driver = target.filename().string();
            }
        }

        // Try to get device name
        std::ifstream label_file(device_path / "label");
        if (label_file.is_open()) {
            std::getline(label_file, gpu.name);
        }

        // Read PCI slot
        gpu.pci_slot = device_path.filename().string();

        if (gpu.name.empty()) {
            gpu.name = "GPU " + std::to_string(gpu.index) +
                       " (" + gpu.driver + ")";
        }

        // Check for VRAM info (AMDGPU)
        fs::path mem_info = device_path / "mem_info_vram_total";
        if (fs::exists(mem_info, ec)) {
            std::ifstream f(mem_info);
            uint64_t total_bytes = 0;
            f >> total_bytes;
            gpu.memory_total_mb = static_cast<float>(total_bytes) / (1024.0f * 1024.0f);
        }

        gpus_.push_back(std::move(gpu));
    }

    // Check if nvidia-smi is available
    FILE* pipe = popen("which nvidia-smi 2>/dev/null", "r");
    if (pipe) {
        char buf[256];
        if (fgets(buf, sizeof(buf), pipe) != nullptr && strlen(buf) > 0) {
            nvidia_available_ = true;
        }
        pclose(pipe);
    }

    if (gpus_.empty() && nvidia_available_) {
        // Add a placeholder for NVIDIA GPU detected via nvidia-smi
        GpuInfo gpu;
        gpu.index = 0;
        gpu.name = "NVIDIA GPU";
        gpu.driver = "nvidia";
        gpus_.push_back(gpu);
    }
}

Result<void, std::string> GpuMonitor::sample() {
    detect();

    if (gpus_.empty()) {
        return Result<void, std::string>::ok();
    }

    // Try nvidia-smi first if available
    if (nvidia_available_) {
        read_nvidia_smi();
    }

    // Read sysfs for each GPU
    for (auto& gpu : gpus_) {
        read_sysfs_gpu(gpu);
    }

    return Result<void, std::string>::ok();
}

void GpuMonitor::read_sysfs_gpu(GpuInfo& gpu) {
    std::string card_path = "/sys/class/drm/card" + std::to_string(gpu.index);
    std::string device_path = card_path + "/device";

    // AMDGPU-specific: look for hwmon
    std::error_code ec;
    std::string hwmon_path;
    fs::path hwmon_dir = fs::path(device_path) / "hwmon";
    if (fs::exists(hwmon_dir, ec)) {
        for (const auto& entry : fs::directory_iterator(hwmon_dir, ec)) {
            hwmon_path = entry.path().string();
            break;
        }
    }

    if (!hwmon_path.empty()) {
        read_amdgpu(gpu, hwmon_path);
    }

    // Try reading GPU busy percent (AMDGPU)
    std::ifstream busy_file(device_path + "/gpu_busy_percent");
    if (busy_file.is_open()) {
        int busy = 0;
        busy_file >> busy;
        gpu.utilization = static_cast<float>(busy);
    }

    // VRAM used
    std::ifstream vram_used_file(device_path + "/mem_info_vram_used");
    if (vram_used_file.is_open()) {
        uint64_t used_bytes = 0;
        vram_used_file >> used_bytes;
        gpu.memory_used_mb = static_cast<float>(used_bytes) / (1024.0f * 1024.0f);
    }

    // Update history
    gpu.util_history.push_back(gpu.utilization);
    while (static_cast<int>(gpu.util_history.size()) > GpuInfo::kMaxHistory) {
        gpu.util_history.pop_front();
    }

    gpu.temp_history.push_back(gpu.temperature);
    while (static_cast<int>(gpu.temp_history.size()) > GpuInfo::kMaxHistory) {
        gpu.temp_history.pop_front();
    }

    float mem_pct = gpu.memory_total_mb > 0
                        ? (gpu.memory_used_mb / gpu.memory_total_mb * 100.0f)
                        : 0.0f;
    gpu.mem_history.push_back(mem_pct);
    while (static_cast<int>(gpu.mem_history.size()) > GpuInfo::kMaxHistory) {
        gpu.mem_history.pop_front();
    }
}

void GpuMonitor::read_amdgpu(GpuInfo& gpu, const std::string& hwmon_path) {
    // Temperature
    std::ifstream temp_file(hwmon_path + "/temp1_input");
    if (temp_file.is_open()) {
        int temp_milli = 0;
        temp_file >> temp_milli;
        gpu.temperature = static_cast<float>(temp_milli) / 1000.0f;
    }

    // Fan speed
    std::ifstream fan_file(hwmon_path + "/pwm1");
    if (fan_file.is_open()) {
        int pwm = 0;
        fan_file >> pwm;
        gpu.fan_speed_pct = static_cast<float>(pwm) / 255.0f * 100.0f;
    }

    // Power
    std::ifstream power_file(hwmon_path + "/power1_average");
    if (power_file.is_open()) {
        uint64_t microwatts = 0;
        power_file >> microwatts;
        gpu.power_watts = static_cast<float>(microwatts) / 1e6f;
    }

    // Clock
    std::ifstream freq_file(hwmon_path + "/freq1_input");
    if (freq_file.is_open()) {
        uint64_t hz = 0;
        freq_file >> hz;
        gpu.clock_mhz = static_cast<int>(hz / 1000000);
    }
}

void GpuMonitor::read_nvidia_smi() {
    // Query nvidia-smi for GPU stats in CSV format
    const char* cmd =
        "nvidia-smi --query-gpu="
        "index,name,temperature.gpu,utilization.gpu,memory.used,memory.total,"
        "power.draw,fan.speed,clocks.gr,clocks.mem "
        "--format=csv,noheader,nounits 2>/dev/null";

    FILE* pipe = popen(cmd, "r");
    if (!pipe) return;

    char line_buf[512];
    while (fgets(line_buf, sizeof(line_buf), pipe)) {
        int idx = 0;
        char name[128] = {};
        float temp = 0, util = 0, mem_used = 0, mem_total = 0;
        float power = 0, fan = 0;
        int gpu_clock = 0, mem_clock = 0;

        int scanned = sscanf(line_buf,
            "%d, %127[^,], %f, %f, %f, %f, %f, %f, %d, %d",
            &idx, name, &temp, &util, &mem_used, &mem_total,
            &power, &fan, &gpu_clock, &mem_clock);

        if (scanned < 6) continue;

        // Find or create GPU entry
        GpuInfo* gpu = nullptr;
        for (auto& g : gpus_) {
            if (g.index == idx) {
                gpu = &g;
                break;
            }
        }
        if (!gpu) {
            gpus_.push_back({});
            gpu = &gpus_.back();
            gpu->index = idx;
        }

        gpu->name = name;
        // Trim leading space from name
        while (!gpu->name.empty() && gpu->name[0] == ' ') {
            gpu->name.erase(gpu->name.begin());
        }
        gpu->driver = "nvidia";
        gpu->temperature = temp;
        gpu->utilization = util;
        gpu->memory_used_mb = mem_used;
        gpu->memory_total_mb = mem_total;
        gpu->power_watts = power;
        gpu->fan_speed_pct = fan;
        gpu->clock_mhz = gpu_clock;
        gpu->mem_clock_mhz = mem_clock;
    }

    pclose(pipe);
}

void GpuMonitor::render() {
    ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.67f, 1.0f), "GPU");

    if (gpus_.empty()) {
        ImGui::TextDisabled("No GPU detected");
        return;
    }

    for (size_t i = 0; i < gpus_.size(); ++i) {
        const auto& gpu = gpus_[i];

        if (gpus_.size() > 1) {
            ImGui::Separator();
        }

        ImGui::Text("%s", gpu.name.c_str());
        ImGui::Text("Driver: %s  |  Clock: %d MHz  |  Mem Clock: %d MHz",
                    gpu.driver.c_str(), gpu.clock_mhz, gpu.mem_clock_mhz);

        // Utilization
        ImGui::Text("Utilization: %.0f%%", gpu.utilization);
        if (!gpu.util_history.empty()) {
            std::vector<float> h(gpu.util_history.begin(),
                                  gpu.util_history.end());
            ImGui::PlotLines("##GPUUtil", h.data(),
                             static_cast<int>(h.size()),
                             0, nullptr, 0.0f, 100.0f, ImVec2(0, 60));
        }

        // VRAM
        if (gpu.memory_total_mb > 0) {
            float mem_pct = gpu.memory_used_mb / gpu.memory_total_mb;
            char overlay[64];
            snprintf(overlay, sizeof(overlay), "%.0f / %.0f MB",
                     gpu.memory_used_mb, gpu.memory_total_mb);
            ImGui::ProgressBar(mem_pct, ImVec2(-1, 20), overlay);
        }

        // Temperature + Power + Fan
        ImGui::Columns(3, "gpuStats", false);
        ImGui::Text("Temp: %.0f C", gpu.temperature);
        ImGui::NextColumn();
        ImGui::Text("Power: %.1f W", gpu.power_watts);
        ImGui::NextColumn();
        ImGui::Text("Fan: %.0f%%", gpu.fan_speed_pct);
        ImGui::NextColumn();
        ImGui::Columns(1);

        // Temperature history
        if (!gpu.temp_history.empty()) {
            std::vector<float> h(gpu.temp_history.begin(),
                                  gpu.temp_history.end());
            ImGui::PlotLines("##GPUTemp", h.data(),
                             static_cast<int>(h.size()),
                             0, "Temperature", 0.0f, 110.0f,
                             ImVec2(0, 40));
        }
    }
}

} // namespace straylight::sysmon
