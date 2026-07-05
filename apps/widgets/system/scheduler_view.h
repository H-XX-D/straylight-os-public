// apps/widgets/system/scheduler_view.h
#pragma once

#include <straylight/widget.h>
#include <straylight/ipc_client.h>
#include <string>
#include <vector>

namespace straylight::widgets {

struct CgroupInfo {
    std::string path;       // e.g. "/sys/fs/cgroup/straylight.slice/ml.scope"
    std::string name;       // leaf name
    int depth = 0;
    int nr_tasks = 0;
    float cpu_usage_pct = 0.0f;
    int64_t memory_current = 0;
    int64_t memory_max = 0;
    std::string cpu_set;    // pinned CPUs
    int64_t io_read_bytes = 0;
    int64_t io_write_bytes = 0;
};

struct TaskPlacement {
    int pid = 0;
    std::string comm;
    int cpu = -1;
    int numa_node = -1;
    std::string cgroup;
    std::string sched_policy; // "normal", "fifo", "rr", "batch", "idle", "deadline"
    int priority = 0;
    float cpu_pct = 0.0f;
};

struct KernelSchedulerStatus {
    bool online = false;
    std::string status_text;
    std::string policy_text;
    std::vector<std::string> task_lines;
};

class SchedulerViewWidget : public WidgetBase {
public:
    const char* name() const override { return "Scheduler View"; }
    float poll_interval() const override { return 2.0f; }
    void update() override;
    void render(bool* p_open) override;

private:
    IpcJsonClient ipc_;
    bool connected_ = false;
    KernelSchedulerStatus kernel_;
    std::vector<CgroupInfo> cgroups_;
    std::vector<TaskPlacement> tasks_;
    std::string error_msg_;
    int view_tab_ = 0;
    char task_filter_[128]{};

    void try_connect();
    void fetch_kernel_scheduler();
    void fetch_cgroups();
    void fetch_tasks();
    static std::string human_bytes(int64_t bytes);
};

} // namespace straylight::widgets
