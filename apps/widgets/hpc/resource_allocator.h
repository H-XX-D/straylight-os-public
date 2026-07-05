// apps/widgets/hpc/resource_allocator.h
#pragma once

#include <straylight/widget.h>
#include <straylight/ipc_client.h>
#include <string>
#include <vector>

namespace straylight::widgets {

struct GpuAllocation {
    int gpu_id = 0;
    std::string device_name;
    std::string assigned_job;
    std::string assigned_user;
    float util_pct = 0.0f;
    float vram_used_mb = 0.0f;
    float vram_total_mb = 0.0f;
    bool reserved = false;
};

struct CpuAllocation {
    int cpu_id = 0;
    int numa_node = 0;
    std::string assigned_job;
    float util_pct = 0.0f;
    bool reserved = false;
};

struct MemAllocation {
    int numa_node = 0;
    float used_gb = 0.0f;
    float total_gb = 0.0f;
    std::string assigned_job;
};

class ResourceAllocatorWidget : public WidgetBase {
public:
    const char* name() const override { return "Resource Allocator"; }
    float poll_interval() const override { return 3.0f; }
    void update() override;
    void render(bool* p_open) override;

private:
    IpcJsonClient ipc_;
    bool connected_ = false;
    std::vector<GpuAllocation> gpu_allocs_;
    std::vector<CpuAllocation> cpu_allocs_;
    std::vector<MemAllocation> mem_allocs_;
    std::string error_msg_;
    int view_tab_ = 0; // 0=GPU, 1=CPU, 2=Memory

    void try_connect();
    void fetch_allocations();
};

} // namespace straylight::widgets
