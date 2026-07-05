#pragma once
#include <imgui.h>
#include <cstdint>
#include <string>
#include <vector>

namespace straylight::pmem {

enum class DimmHealth { Healthy, Warning, Critical };

struct DimmInfo {
    int          slot;
    std::string  serial;
    std::string  part_number;
    uint64_t     capacity_gb;
    uint64_t     used_gb;
    float        read_bw_gbs;
    float        write_bw_gbs;
    float        latency_ns;
    float        temp_c;
    uint64_t     media_errors;
    DimmHealth   health;
    bool         dax_enabled;
    std::string  mode;
};

struct RegionInfo {
    int          id;
    std::string  type;
    uint64_t     size_gb;
    uint64_t     free_gb;
    int          dimm_count;
    bool         interleaved;
    std::string  namespace_dev;
};

struct NamespaceInfo {
    std::string  dev;
    std::string  name;
    uint64_t     size_gb;
    std::string  mode;
    bool         mounted;
    std::string  mount_point;
    float        iops_k;
};

struct PmemState {
    std::vector<DimmInfo>      dimms;
    std::vector<RegionInfo>    regions;
    std::vector<NamespaceInfo> namespaces;
    int active_tab = 0;
    float read_history[128]  = {};
    float write_history[128] = {};
    int   bw_offset = 0;

    void init();
};

class PmemPanel {
public:
    void init() { state_.init(); }
    void render();
private:
    PmemState state_;
    void render_devices_tab();
    void render_regions_tab();
    void render_namespaces_tab();
    void render_perf_tab();
};

} // namespace straylight::pmem
