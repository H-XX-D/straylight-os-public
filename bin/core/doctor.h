// bin/core/doctor.h
#pragma once

#include <straylight/types.h>
#include <mutex>
#include <string>
#include <unordered_map>

namespace straylight {

class Doctor {
public:
    void record_health(const std::string& name, HealthStatus status);
    bool all_healthy() const;
    size_t unhealthy_count() const;
    bool needs_restart(const std::string& name) const;

    static constexpr int kRestartThreshold = 3;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, HealthStatus> health_;
    std::unordered_map<std::string, int> fail_streak_;
};

} // namespace straylight
