// bin/scheduler/cgroup.h
#pragma once

#include <straylight/result.h>
#include <straylight/error.h>
#include <filesystem>
#include <string>

namespace straylight {

/// Read/write cgroup v2 knobs for a single cgroup directory.
class CgroupV2 {
public:
    explicit CgroupV2(std::filesystem::path path) : path_(std::move(path)) {}

    Result<unsigned, SLError> read_cpu_weight() const;
    Result<void, SLError> set_cpu_weight(unsigned weight) const;
    Result<void, SLError> set_memory_max(size_t bytes) const;
    Result<void, SLError> set_cpuset_cpus(const std::string& cpus) const;
    Result<void, SLError> set_cpuset_mems(const std::string& mems) const;

private:
    std::filesystem::path path_;

    Result<void, SLError> write_control_file(const char* name,
                                             const std::string& value) const;
};

} // namespace straylight
