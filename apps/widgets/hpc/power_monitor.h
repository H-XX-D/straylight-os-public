// apps/widgets/hpc/power_monitor.h
#pragma once

#include <straylight/widget.h>
#include <array>
#include <string>
#include <vector>

namespace straylight::widgets {

struct RaplDomain {
    std::string name;    // "package-0", "core", "uncore", "dram", "psys"
    std::string path;    // sysfs path
    float power_w = 0.0f;
    float max_power_w = 0.0f;
    uint64_t energy_uj = 0;
    uint64_t prev_energy_uj = 0;
    uint64_t max_energy_range_uj = 0;
    // History
    static constexpr int kHistLen = 120;
    std::array<float, kHistLen> power_hist{};
    int hist_offset = 0;
};

struct GpuPower {
    int gpu_id = 0;
    std::string name;
    float power_w = 0.0f;
    float limit_w = 0.0f;
};

class PowerMonitorWidget : public WidgetBase {
public:
    const char* name() const override { return "Power Monitor"; }
    float poll_interval() const override { return 1.0f; }
    void update() override;
    void render(bool* p_open) override;

private:
    std::vector<RaplDomain> rapl_domains_;
    std::vector<GpuPower> gpu_powers_;
    bool rapl_available_ = false;
    float total_cpu_power_ = 0.0f;
    float total_gpu_power_ = 0.0f;
    std::chrono::steady_clock::time_point last_energy_sample_{};

    void discover_rapl();
    void read_rapl();
    void read_gpu_power();
    void push_history(RaplDomain& d);
};

} // namespace straylight::widgets
