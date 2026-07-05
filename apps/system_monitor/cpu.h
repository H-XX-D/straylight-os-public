// apps/system_monitor/cpu.h
// CPU usage monitoring via /proc/stat
#pragma once

#include <straylight/result.h>

#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace straylight::sysmon {

/// Raw CPU time counters from /proc/stat.
struct CpuTimes {
    uint64_t user = 0;
    uint64_t nice = 0;
    uint64_t system = 0;
    uint64_t idle = 0;
    uint64_t iowait = 0;
    uint64_t irq = 0;
    uint64_t softirq = 0;
    uint64_t steal = 0;

    [[nodiscard]] uint64_t total() const {
        return user + nice + system + idle + iowait + irq + softirq + steal;
    }

    [[nodiscard]] uint64_t active() const {
        return total() - idle - iowait;
    }
};

/// Per-core usage data.
struct CoreUsage {
    int core_id = 0;
    float usage_percent = 0.0f; // 0..100
    std::deque<float> history;  // Past N samples

    static constexpr int kMaxHistory = 60;
};

/// Overall CPU information.
struct CpuInfo {
    std::string model_name;
    int core_count = 0;
    float total_usage = 0.0f;
    std::deque<float> total_history;
    std::vector<CoreUsage> cores;
    float frequency_mhz = 0.0f;
    float temperature = 0.0f; // Celsius, from hwmon

    static constexpr int kMaxHistory = 60;
};

/// CPU monitor — reads /proc/stat and calculates usage.
class CpuMonitor {
public:
    CpuMonitor();

    /// Sample current CPU usage. Call once per update cycle.
    Result<void, std::string> sample();

    /// Get the current CPU info.
    [[nodiscard]] const CpuInfo& info() const { return info_; }

    /// Render CPU tab in ImGui.
    void render();

private:
    Result<void, std::string> read_proc_stat();
    void read_cpu_info();
    void read_temperature();
    void read_frequency();

    CpuInfo info_;

    // Previous sample for delta calculation
    CpuTimes prev_total_;
    std::vector<CpuTimes> prev_cores_;
    bool first_sample_ = true;
};

} // namespace straylight::sysmon
