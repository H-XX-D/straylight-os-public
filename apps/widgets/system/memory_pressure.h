// apps/widgets/system/memory_pressure.h
#pragma once

#include <straylight/widget.h>
#include <imgui.h>
#include <array>
#include <string>

namespace straylight::widgets {

struct PsiMetrics {
    // avg10, avg60, avg300 are percentages; total is microseconds
    float some_avg10 = 0, some_avg60 = 0, some_avg300 = 0;
    uint64_t some_total = 0;
    float full_avg10 = 0, full_avg60 = 0, full_avg300 = 0;
    uint64_t full_total = 0;
};

class MemoryPressureWidget : public WidgetBase {
public:
    const char* name() const override { return "Memory Pressure"; }
    float poll_interval() const override { return 1.0f; }
    void update() override;
    void render(bool* p_open) override;

private:
    PsiMetrics mem_psi_;
    PsiMetrics cpu_psi_;
    PsiMetrics io_psi_;
    bool psi_available_ = false;

    // Memory info from /proc/meminfo
    float mem_total_mb_ = 0;
    float mem_available_mb_ = 0;
    float mem_used_mb_ = 0;
    float swap_total_mb_ = 0;
    float swap_used_mb_ = 0;
    float cached_mb_ = 0;
    float buffers_mb_ = 0;
    float slab_mb_ = 0;

    // History
    static constexpr int kHistLen = 120;
    std::array<float, kHistLen> mem_psi_hist_{};
    std::array<float, kHistLen> mem_used_hist_{};
    int hist_offset_ = 0;

    void read_psi();
    void read_meminfo();
    static PsiMetrics parse_psi_file(const std::string& path);
    static ImVec4 pressure_color(float avg10);
};

} // namespace straylight::widgets
