// apps/hub/gpu_panel.h
// GPU management panel — per-GPU cards, temp, utilization, VRAM, power, fan speed.
#pragma once

#include <imgui.h>

#include <string>
#include <cstdint>
#include <vector>

namespace straylight::hub {

struct GpuInfo {
    int index = 0;
    std::string name;
    std::string driver;

    float utilization = 0.0f;      // 0.0 - 1.0
    float memory_usage = 0.0f;     // 0.0 - 1.0
    uint64_t memory_used_mb = 0;
    uint64_t memory_total_mb = 0;

    int temperature_c = 0;
    int power_draw_w = 0;
    int power_limit_w = 0;
    int fan_speed_pct = 0;

    int clock_core_mhz = 0;
    int clock_memory_mhz = 0;

    std::string pci_bus;

    // History for mini sparklines
    static constexpr int HISTORY_LEN = 60;
    float util_history[HISTORY_LEN]{};
    float temp_history[HISTORY_LEN]{};
    float mem_history[HISTORY_LEN]{};
    int history_idx = 0;
};

class GpuPanel {
public:
    GpuPanel() = default;

    /// Detect and sample all GPUs.
    void sample();

    /// Render the GPU tab.
    void render();

private:
    std::vector<GpuInfo> gpus_;
    bool detected_ = false;

    void detect_gpus();
    void sample_nvidia(GpuInfo& gpu);
    void sample_amd(GpuInfo& gpu);
    void sample_intel(GpuInfo& gpu);

    void render_gpu_card(GpuInfo& gpu);
};

} // namespace straylight::hub
