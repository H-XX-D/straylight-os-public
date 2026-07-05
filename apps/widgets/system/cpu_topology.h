// apps/widgets/system/cpu_topology.h
#pragma once

#include <straylight/widget.h>
#include <array>
#include <string>
#include <vector>

namespace straylight::widgets {

struct CpuCore {
    int cpu_id = 0;
    int core_id = 0;
    int package_id = 0;
    int numa_node = 0;
    std::string core_type; // "performance", "efficiency", or ""
    float freq_mhz = 0.0f;
    float max_freq_mhz = 0.0f;
    float min_freq_mhz = 0.0f;
    float temperature_c = 0.0f;
    float utilization_pct = 0.0f;
    uint64_t prev_total = 0;
    uint64_t prev_idle = 0;
};

struct NumaNode {
    int id = 0;
    float mem_total_mb = 0.0f;
    float mem_free_mb = 0.0f;
    std::vector<int> cpu_ids;
};

class CpuTopologyWidget : public WidgetBase {
public:
    const char* name() const override { return "CPU Topology"; }
    float poll_interval() const override { return 1.0f; }
    void update() override;
    void render(bool* p_open) override;

    // Exposed for testing
    static std::vector<CpuCore> parse_proc_cpuinfo(const std::string& content);

private:
    std::vector<CpuCore> cores_;
    std::vector<NumaNode> numa_nodes_;
    std::string model_name_;
    int num_sockets_ = 0;
    bool topology_discovered_ = false;
    int view_mode_ = 0; // 0=grid, 1=table

    void discover_topology();
    void read_frequencies();
    void read_temperatures();
    void read_utilization();
    void read_numa();
};

} // namespace straylight::widgets
