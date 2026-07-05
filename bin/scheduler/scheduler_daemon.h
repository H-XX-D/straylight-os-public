// bin/scheduler/scheduler_daemon.h
#pragma once

#include <straylight/common.h>
#include <straylight/daemon.h>
#include "topology.h"
#include "cgroup.h"

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace straylight {

enum class Priority { High, Normal, Low };

struct NumaPlacement {
    std::string cpus;
    std::string mems;
};

/// Maps subsystem names to scheduling priorities and resolves CPU weights.
class PriorityQueue {
public:
    void enqueue(const std::string& name, Priority prio);
    unsigned cpu_weight(const std::string& name) const;
    const std::unordered_map<std::string, Priority>& entries() const { return entries_; }

private:
    std::unordered_map<std::string, Priority> entries_;
};

/// Userspace task scheduler managing cgroup v2 resource allocation.
/// Priority queue assigns CPU weight and memory limits per subsystem.
/// Communicates with straylight-scheduler.ko via /proc/straylight/sched
/// when the kernel module is loaded (graceful degradation if absent).
class SchedulerDaemon : public DaemonBase {
public:
    Result<void, SLError> init(const Config& cfg) override;
    Result<void, SLError> tick() override;
    void shutdown() override;

    void register_subsystem(const std::string& name, Priority prio);

private:
    PriorityQueue queue_;
    Topology topology_;
    bool kernel_module_available_ = false;
    std::mutex mutex_;  // Guards queue_ and apply_priorities()
    std::unordered_map<std::string, NumaPlacement> placements_;

    void apply_priorities();
    void load_numa_policy();
    void register_numa_placement(const std::string& name,
                                 const std::string& profile);
};

} // namespace straylight
