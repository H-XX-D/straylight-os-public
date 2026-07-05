// bin/scheduler/scheduler_daemon.cpp
#include "scheduler_daemon.h"

#include <straylight/log.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

namespace straylight {

// ---------------------------------------------------------------------------
// PriorityQueue
// ---------------------------------------------------------------------------

namespace {
constexpr unsigned kWeightHigh   = 800;
constexpr unsigned kWeightNormal = 100;
constexpr unsigned kWeightLow    = 10;

unsigned weight_for(Priority p) {
    switch (p) {
        case Priority::High:   return kWeightHigh;
        case Priority::Normal: return kWeightNormal;
        case Priority::Low:    return kWeightLow;
    }
    return kWeightNormal;
}
} // anonymous namespace

void PriorityQueue::enqueue(const std::string& name, Priority prio) {
    entries_[name] = prio;
}

unsigned PriorityQueue::cpu_weight(const std::string& name) const {
    auto it = entries_.find(name);
    if (it == entries_.end()) return kWeightNormal;
    return weight_for(it->second);
}

// ---------------------------------------------------------------------------
// SchedulerDaemon
// ---------------------------------------------------------------------------

static constexpr const char* kCgroupBase = "/sys/fs/cgroup/straylight";
static constexpr const char* kKernelModulePath = "/proc/straylight/sched";
static constexpr const char* kNumaConfigPath = "/etc/straylight/numa.conf";

std::string trim_copy(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    auto b = std::find_if(s.begin(), s.end(), not_space);
    auto e = std::find_if(s.rbegin(), s.rend(), not_space).base();
    if (b >= e) return {};
    return std::string(b, e);
}

std::unordered_map<std::string, std::string> read_key_values(const char* path) {
    std::unordered_map<std::string, std::string> values;
    std::ifstream f(path);
    std::string line;

    while (std::getline(f, line)) {
        auto hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        auto key = trim_copy(line.substr(0, eq));
        auto val = trim_copy(line.substr(eq + 1));
        if (val.size() >= 2 &&
            ((val.front() == '"' && val.back() == '"') ||
             (val.front() == '\'' && val.back() == '\''))) {
            val = val.substr(1, val.size() - 2);
        }
        if (!key.empty()) values[key] = val;
    }

    return values;
}

Result<void, SLError> SchedulerDaemon::init(const Config& /*cfg*/) {
    SL_INFO("scheduler: initializing");

    // Parse CPU topology
    {
        std::ifstream f("/proc/cpuinfo");
        if (f.is_open()) {
            std::ostringstream buf;
            buf << f.rdbuf();
            auto res = topology_.parse_cpuinfo(buf.str());
            if (res.has_value()) {
                SL_INFO("scheduler: detected {} logical CPUs, {} physical cores",
                        topology_.logical_cpu_count(),
                        topology_.physical_core_count());
            } else {
                SL_WARN("scheduler: failed to parse cpuinfo: {}", res.error().message());
            }
        } else {
            SL_WARN("scheduler: /proc/cpuinfo not available (not running on Linux?)");
        }
    }

    // Create cgroup hierarchy base directory
    {
        std::error_code ec;
        std::filesystem::create_directories(kCgroupBase, ec);
        if (ec) {
            SL_WARN("scheduler: cannot create cgroup dir {}: {}", kCgroupBase, ec.message());
        }
    }

    // Probe for kernel module
    kernel_module_available_ = std::filesystem::exists(kKernelModulePath);
    if (kernel_module_available_) {
        SL_INFO("scheduler: kernel module detected at {}", kKernelModulePath);
    } else {
        SL_INFO("scheduler: kernel module not present, running in userspace-only mode");
    }

    // Register default subsystems
    register_subsystem("straylight-bus",      Priority::High);
    register_subsystem("straylight-registry", Priority::High);
    register_subsystem("straylight-entropy",  Priority::Normal);
    register_subsystem("straylight-fuse",     Priority::Normal);
    register_subsystem("straylight-agent",    Priority::Low);
    register_subsystem("straylight-fabric",   Priority::High);
    register_subsystem("straylight-mesh",     Priority::High);
    register_subsystem("straylight-predict",  Priority::Normal);
    register_subsystem("straylight-whisper",  Priority::Normal);
    register_subsystem("straylight-weave",    Priority::Normal);

    load_numa_policy();

    // Apply initial priorities
    apply_priorities();

    SL_INFO("scheduler: initialization complete");
    return Result<void, SLError>::ok();
}

Result<void, SLError> SchedulerDaemon::tick() {
    std::this_thread::sleep_for(std::chrono::seconds(5));

    // Re-read cgroup stats and reapply if needed
    for (const auto& [name, prio] : queue_.entries()) {
        auto subsys_path = std::filesystem::path(kCgroupBase) / name;
        if (!std::filesystem::exists(subsys_path)) continue;

        CgroupV2 cg(subsys_path);
        auto weight_result = cg.read_cpu_weight();
        if (weight_result.has_value()) {
            unsigned expected = queue_.cpu_weight(name);
            if (weight_result.value() != expected) {
                SL_WARN("scheduler: {} cpu.weight drifted ({} -> {}), correcting",
                        name, weight_result.value(), expected);
                cg.set_cpu_weight(expected);
            }
        }
    }

    return Result<void, SLError>::ok();
}

void SchedulerDaemon::shutdown() {
    SL_INFO("scheduler: shutting down");
}

void SchedulerDaemon::register_subsystem(const std::string& name, Priority prio) {
    std::lock_guard lock(mutex_);
    queue_.enqueue(name, prio);
    SL_DEBUG("scheduler: registered subsystem '{}' at priority {}",
             name, static_cast<int>(prio));
}

void SchedulerDaemon::load_numa_policy() {
    auto kv = read_key_values(kNumaConfigPath);
    if (kv.empty()) {
        SL_INFO("scheduler: no NUMA policy at {}, cpuset placement disabled",
                kNumaConfigPath);
        return;
    }

    auto profile_cpus = [&](const std::string& profile) {
        auto it = kv.find("STRAYLIGHT_NUMA_PROFILE_" + profile + "_CPUS");
        return it == kv.end() ? std::string{} : it->second;
    };
    auto profile_mems = [&](const std::string& profile) {
        auto it = kv.find("STRAYLIGHT_NUMA_PROFILE_" + profile + "_MEMS");
        return it == kv.end() ? std::string{} : it->second;
    };

    auto bind = [&](const std::string& service, const std::string& profile) {
        NumaPlacement p{profile_cpus(profile), profile_mems(profile)};
        if (!p.cpus.empty() || !p.mems.empty()) {
            placements_[service] = p;
            SL_INFO("scheduler: NUMA profile {} -> {} cpus={} mems={}",
                    profile, service, p.cpus, p.mems);
        }
    };

    bind("straylight-scheduler", "control");
    bind("straylight-bus", "control");
    bind("straylight-registry", "control");
    bind("straylight-fabric", "vpu");
    bind("straylight-mesh", "vpu");
    bind("straylight-fuse", "vpu");
    bind("straylight-predict", "vpu");
    bind("straylight-whisper", "vpu");
    bind("straylight-weave", "vpu");
    bind("straylight-agent", "garden");
}

void SchedulerDaemon::apply_priorities() {
    for (const auto& [name, prio] : queue_.entries()) {
        auto subsys_path = std::filesystem::path(kCgroupBase) / name;

        // Create per-subsystem cgroup directory
        std::error_code ec;
        std::filesystem::create_directories(subsys_path, ec);
        if (ec) {
            SL_WARN("scheduler: cannot create cgroup dir for {}: {}", name, ec.message());
            continue;
        }

        // Set cpu.weight
        CgroupV2 cg(subsys_path);
        unsigned weight = queue_.cpu_weight(name);
        auto res = cg.set_cpu_weight(weight);
        if (!res.has_value()) {
            SL_WARN("scheduler: failed to set cpu.weight for {}: {}", name, res.error().message());
        } else {
            SL_DEBUG("scheduler: set {} cpu.weight = {}", name, weight);
        }

        auto placement = placements_.find(name);
        if (placement != placements_.end()) {
            if (!placement->second.mems.empty()) {
                auto mem_res = cg.set_cpuset_mems(placement->second.mems);
                if (!mem_res.has_value()) {
                    SL_WARN("scheduler: failed to set {} cpuset.mems={} : {}",
                            name, placement->second.mems, mem_res.error().message());
                }
            }
            if (!placement->second.cpus.empty()) {
                auto cpu_res = cg.set_cpuset_cpus(placement->second.cpus);
                if (!cpu_res.has_value()) {
                    SL_WARN("scheduler: failed to set {} cpuset.cpus={} : {}",
                            name, placement->second.cpus, cpu_res.error().message());
                }
            }
        }
    }
}

} // namespace straylight
