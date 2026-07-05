// tools/cgroup/cgroup_manager.h
// Cgroup v2 inspector/manager for StrayLight OS.
#pragma once

#include <straylight/result.h>

#include <cstdint>
#include <string>
#include <vector>

namespace straylight {

/// Resource usage for a cgroup.
struct CgroupUsage {
    std::string path;
    uint64_t    cpu_usage_us = 0;
    uint64_t    memory_current = 0;
    uint64_t    memory_limit = 0;
    uint64_t    io_read_bytes = 0;
    uint64_t    io_write_bytes = 0;
    int         process_count = 0;
    double      cpu_percent = 0;
    double      memory_percent = 0;
};

/// Cgroup tree node for visualization.
struct CgroupNode {
    std::string name;
    std::string full_path;
    int         depth = 0;
    int         process_count = 0;
    uint64_t    memory_current = 0;
    std::vector<CgroupNode> children;
};

/// Cgroup resource limits.
struct CgroupLimits {
    int64_t     cpu_max = -1;
    int64_t     cpu_period = 100000;
    int64_t     memory_max = -1;
    int64_t     memory_high = -1;
    std::string io_max;
    int         pids_max = -1;
};

class CgroupManager {
public:
    CgroupManager();
    ~CgroupManager();

    Result<CgroupNode, std::string> tree() const;
    Result<CgroupUsage, std::string> info(const std::string& cgroup_path) const;
    Result<void, std::string> create(const std::string& path);
    Result<void, std::string> remove(const std::string& path);
    Result<void, std::string> move(int pid, const std::string& cgroup_path);
    Result<void, std::string> set_limits(const std::string& cgroup_path, const CgroupLimits& limits);
    Result<CgroupLimits, std::string> get_limits(const std::string& cgroup_path) const;
    Result<std::vector<CgroupUsage>, std::string> usage() const;

private:
    static constexpr const char* CGROUP_ROOT = "/sys/fs/cgroup";
    std::string resolve_path(const std::string& cgroup_path) const;
    Result<std::string, std::string> read_file(const std::string& path) const;
    Result<void, std::string> write_file(const std::string& path, const std::string& value) const;
    CgroupNode build_tree(const std::string& path, int depth) const;
    int count_procs(const std::string& cgroup_full_path) const;
    uint64_t read_uint64(const std::string& path) const;
};

} // namespace straylight
