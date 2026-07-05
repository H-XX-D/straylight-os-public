// bin/core/pipeline.h
#pragma once

#include <straylight/types.h>
#include <string>
#include <vector>

namespace straylight {

enum class SubsystemPriority { Critical, Normal, Low };

struct SubsystemEntry {
    std::string name;
    SubsystemPriority priority;
    HealthStatus last_health = HealthStatus::Unknown;
};

class Pipeline {
public:
    void register_subsystem(const std::string& name, SubsystemPriority prio);
    size_t subsystem_count() const;
    size_t critical_count() const;
    std::vector<SubsystemEntry>& subsystems();
    const std::vector<SubsystemEntry>& subsystems() const;

private:
    std::vector<SubsystemEntry> subsystems_;
};

} // namespace straylight
