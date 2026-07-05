// apps/system_monitor/gpu.h
// GPU monitoring via sysfs and nvidia-smi
#pragma once

#include <straylight/result.h>

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace straylight::sysmon {

/// GPU information for a single GPU device.
struct GpuInfo {
    std::string name;
    std::string driver;
    std::string pci_slot;
    int index = 0;

    float utilization = 0.0f;     // 0..100
    float memory_used_mb = 0.0f;
    float memory_total_mb = 0.0f;
    float temperature = 0.0f;     // Celsius
    float power_watts = 0.0f;
    float fan_speed_pct = 0.0f;
    int clock_mhz = 0;
    int mem_clock_mhz = 0;

    std::deque<float> util_history;
    std::deque<float> temp_history;
    std::deque<float> mem_history;

    static constexpr int kMaxHistory = 60;
};

/// GPU monitor — reads sysfs and nvidia-smi.
class GpuMonitor {
public:
    GpuMonitor();

    /// Detect available GPUs.
    void detect();

    /// Sample current GPU stats.
    Result<void, std::string> sample();

    /// Get GPU info list.
    [[nodiscard]] const std::vector<GpuInfo>& gpus() const { return gpus_; }

    /// Render GPU tab in ImGui.
    void render();

private:
    void read_sysfs_gpu(GpuInfo& gpu);
    void read_nvidia_smi();
    void read_amdgpu(GpuInfo& gpu, const std::string& hwmon_path);

    std::vector<GpuInfo> gpus_;
    bool nvidia_available_ = false;
    bool detected_ = false;
};

} // namespace straylight::sysmon
