#pragma once
// apps/settings/pages/performance.h
#include "../settings_page.h"
#include <string>
#include <vector>

namespace straylight::settings {

struct CpuCore {
    int         core_id;
    int         socket;
    int         cluster;
    int         freq_mhz;
    int         freq_max_mhz;
    float       util_pct;
    float       temp_c;
    bool        online;
    std::string governor;
};

struct FreqScalingConfig {
    int  governor_idx;    // 0=performance 1=powersave 2=schedutil 3=ondemand
    int  min_freq_idx;    // index into freq_steps
    int  max_freq_idx;
    bool boost_enabled;
    bool hwp_enabled;
};

struct HugepageConfig {
    bool transparent_hp;
    int  thp_mode_idx;    // 0=always 1=madvise 2=never
    int  huge_2mb_count;
    int  huge_1gb_count;
};

class PerformancePage : public SettingsPage {
public:
    [[nodiscard]] const char* label() const override { return "Performance"; }
    void load()   override;
    void render() override;

private:
    void render_topology_tab();
    void render_freq_tab();
    void render_hugepages_tab();

    std::vector<CpuCore> cores_;
    FreqScalingConfig    freq_cfg_{};
    HugepageConfig       hp_cfg_{};
    float                core_util_history_[16][32]{};
    int                  history_offset_{0};
    int                  active_tab_{0};
};

} // namespace straylight::settings
