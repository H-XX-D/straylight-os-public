// apps/widgets/ml/gpu_hud.h
#pragma once

#include <straylight/widget.h>
#include <array>
#include <string>
#include <vector>

namespace straylight::widgets {

struct GpuInfo {
    std::string name;
    int index = 0;
    float utilization_pct = 0.0f;   // GPU core %
    float memory_used_mb = 0.0f;
    float memory_total_mb = 0.0f;
    float temperature_c = 0.0f;
    float power_draw_w = 0.0f;
    float power_limit_w = 0.0f;
    int clock_graphics_mhz = 0;
    int clock_memory_mhz = 0;
    int fan_speed_pct = 0;
    // History ring buffer for sparkline
    static constexpr int kHistoryLen = 120;
    std::array<float, kHistoryLen> util_history{};
    std::array<float, kHistoryLen> temp_history{};
    std::array<float, kHistoryLen> vram_history{};
    int history_offset = 0;
};

class GpuHudWidget : public WidgetBase {
public:
    const char* name() const override { return "GPU HUD"; }
    float poll_interval() const override { return 0.5f; }
    void update() override;
    void render(bool* p_open) override;

    // Exposed for testing
    static std::vector<GpuInfo> parse_nvidia_smi(const std::string& csv_output);
    static std::vector<GpuInfo> parse_sysfs_amd();

private:
    std::vector<GpuInfo> gpus_;
    bool nvidia_available_ = false;
    bool amd_available_ = false;
    int selected_gpu_ = 0;

    void read_nvidia_smi();
    void read_sysfs();
    void push_history(GpuInfo& gpu);
};

} // namespace straylight::widgets
