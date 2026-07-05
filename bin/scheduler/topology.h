// bin/scheduler/topology.h
#pragma once

#include <straylight/result.h>
#include <straylight/error.h>
#include <string>

namespace straylight {

/// Parses CPU topology from /proc/cpuinfo-style text.
class Topology {
public:
    Result<void, SLError> parse_cpuinfo(const std::string& content);
    unsigned logical_cpu_count() const { return logical_count_; }
    unsigned physical_core_count() const { return physical_cores_; }

private:
    unsigned logical_count_ = 0;
    unsigned physical_cores_ = 0;
};

} // namespace straylight
