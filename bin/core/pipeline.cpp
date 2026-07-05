// bin/core/pipeline.cpp
#include "pipeline.h"
#include <algorithm>

namespace straylight {

void Pipeline::register_subsystem(const std::string& name, SubsystemPriority prio) {
    subsystems_.push_back({name, prio, HealthStatus::Unknown});
}

size_t Pipeline::subsystem_count() const {
    return subsystems_.size();
}

size_t Pipeline::critical_count() const {
    return static_cast<size_t>(std::count_if(
        subsystems_.begin(), subsystems_.end(),
        [](const SubsystemEntry& e) {
            return e.priority == SubsystemPriority::Critical;
        }));
}

std::vector<SubsystemEntry>& Pipeline::subsystems() {
    return subsystems_;
}

const std::vector<SubsystemEntry>& Pipeline::subsystems() const {
    return subsystems_;
}

} // namespace straylight
