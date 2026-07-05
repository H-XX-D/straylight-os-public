// bin/core/doctor.cpp
#include "doctor.h"

namespace straylight {

void Doctor::record_health(const std::string& name, HealthStatus status) {
    std::lock_guard<std::mutex> lock(mutex_);
    health_[name] = status;

    if (status == HealthStatus::Degraded || status == HealthStatus::Failed) {
        fail_streak_[name]++;
    } else if (status == HealthStatus::Healthy) {
        fail_streak_[name] = 0;
    }
}

bool Doctor::all_healthy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [name, status] : health_) {
        if (status != HealthStatus::Healthy) {
            return false;
        }
    }
    return true;
}

size_t Doctor::unhealthy_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    for (const auto& [name, status] : health_) {
        if (status != HealthStatus::Healthy) {
            ++count;
        }
    }
    return count;
}

bool Doctor::needs_restart(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = fail_streak_.find(name);
    if (it == fail_streak_.end()) {
        return false;
    }
    return it->second >= kRestartThreshold;
}

} // namespace straylight
