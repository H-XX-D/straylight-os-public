// apps/system_monitor/memory.h
// Memory usage monitoring via /proc/meminfo
#pragma once

#include <straylight/result.h>

#include <cstdint>
#include <deque>
#include <string>

namespace straylight::sysmon {

/// Memory statistics from /proc/meminfo.
struct MemoryInfo {
    uint64_t total_kb = 0;
    uint64_t free_kb = 0;
    uint64_t available_kb = 0;
    uint64_t buffers_kb = 0;
    uint64_t cached_kb = 0;
    uint64_t slab_kb = 0;
    uint64_t swap_total_kb = 0;
    uint64_t swap_free_kb = 0;

    [[nodiscard]] uint64_t used_kb() const {
        return total_kb - available_kb;
    }
    [[nodiscard]] uint64_t swap_used_kb() const {
        return swap_total_kb - swap_free_kb;
    }
    [[nodiscard]] float used_percent() const {
        return total_kb > 0 ? static_cast<float>(used_kb()) /
                              static_cast<float>(total_kb) * 100.0f : 0.0f;
    }
    [[nodiscard]] float swap_used_percent() const {
        return swap_total_kb > 0
                   ? static_cast<float>(swap_used_kb()) /
                         static_cast<float>(swap_total_kb) * 100.0f
                   : 0.0f;
    }

    std::deque<float> usage_history;
    std::deque<float> swap_history;

    static constexpr int kMaxHistory = 60;
};

/// Memory monitor — reads /proc/meminfo.
class MemoryMonitor {
public:
    MemoryMonitor();

    /// Sample current memory usage.
    Result<void, std::string> sample();

    /// Get the current memory info.
    [[nodiscard]] const MemoryInfo& info() const { return info_; }

    /// Render memory tab in ImGui.
    void render();

private:
    MemoryInfo info_;
};

/// Format KB to human-readable string.
std::string format_kb(uint64_t kb);

} // namespace straylight::sysmon
