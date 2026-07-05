#pragma once
// apps/settings/pages/system.h
#include "../settings_page.h"
#include <string>
#include <vector>

namespace straylight::settings {

struct DaemonStatus {
    std::string name;
    int         pid;
    int         uptime_s;
    bool        running;
    float       cpu_pct;
    float       mem_mb;
};

struct SchedulerConfig {
    int  policy_idx;           // 0=CFS 1=FIFO 2=RR
    int  tick_hz;              // 100/250/1000
    bool cgroup_v2;
    bool cpu_isolation;
    char isolated_cpus[64];
};

struct RegistryEntry {
    std::string key;
    std::string value;
    std::string type;          // "string" "int" "bool" "float"
    bool        editable;
};

class SystemPage : public SettingsPage {
public:
    [[nodiscard]] const char* label() const override { return "System"; }
    void load()   override;
    void render() override;

private:
    void render_daemons_tab();
    void render_scheduler_tab();
    void render_registry_tab();

    std::vector<DaemonStatus>  daemons_;
    SchedulerConfig            sched_{};
    std::vector<RegistryEntry> registry_;
    char                       reg_search_[128]{};
    int                        active_tab_{0};
};

} // namespace straylight::settings
