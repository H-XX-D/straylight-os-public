#pragma once
#include <imgui.h>
#include <cstdint>
#include <string>
#include <vector>

namespace straylight::rhem {

enum class TierType { DRAM, HBM, PMEM, CXL, NVMe };

struct MemTier {
    TierType     type;
    std::string  label;
    uint64_t     total_gb;
    uint64_t     used_gb;
    float        read_bw_mbs;
    float        write_bw_mbs;
    float        latency_ns;
    float        cost_per_gb;
};

struct AllocPolicy {
    std::string name;
    std::string description;
    int         demotion_timeout_s;
    int         hbm_fill_target_pct;
};

struct AllocationEntry {
    std::string  name;
    uint64_t     size_mb;
    TierType     current_tier;
    bool         hot;
    bool         migrating;
    float        migration_pct;
    int          numa_node;
};

struct RhemState {
    std::vector<MemTier>         tiers;
    std::vector<AllocPolicy>     policies;
    std::vector<AllocationEntry> allocations;
    int   active_tab   = 0;
    int   active_policy = 0;
    float total_bw_gbs = 0.0f;
    float avg_lat_ns   = 0.0f;
    float efficiency   = 0.0f;
    float bw_history[128] = {};
    int   bw_offset    = 0;

    void init();
};

class RhemPanel {
public:
    void init() { state_.init(); }
    void render();
private:
    RhemState state_;
    void render_topology_tab();
    void render_allocations_tab();
    void render_policy_tab();
    void render_migration_tab();
};

} // namespace straylight::rhem
