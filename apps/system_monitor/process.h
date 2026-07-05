// apps/system_monitor/process.h
// Process list monitoring via /proc/[pid]/stat
#pragma once

#include <straylight/result.h>

#include <cstdint>
#include <string>
#include <sys/types.h>
#include <vector>

namespace straylight::sysmon {

/// Process info from /proc/[pid]/stat.
struct ProcessInfo {
    pid_t pid = 0;
    pid_t ppid = 0;
    std::string comm;
    char state = '?';
    std::string username;

    float cpu_percent = 0.0f;
    float mem_percent = 0.0f;
    uint64_t vsize_kb = 0;   // Virtual memory
    uint64_t rss_kb = 0;     // Resident set size
    int threads = 0;
    int nice = 0;
    int priority = 0;

    // Raw times for delta calculation
    uint64_t utime = 0;
    uint64_t stime = 0;
    uint64_t starttime = 0;
};

/// Sort criteria for process list.
enum class ProcessSort {
    Pid,
    Name,
    Cpu,
    Memory,
    State,
};

/// Process monitor.
class ProcessMonitor {
public:
    ProcessMonitor();

    /// Sample current process list.
    Result<void, std::string> sample();

    /// Get the process list.
    [[nodiscard]] const std::vector<ProcessInfo>& processes() const {
        return processes_;
    }

    /// Sort settings.
    void set_sort(ProcessSort sort, bool ascending = true);

    /// Filter by name.
    void set_filter(const std::string& filter);

    /// Send a signal to a process.
    Result<void, std::string> kill_process(pid_t pid, int signal);

    /// Render processes tab in ImGui.
    void render();

private:
    void read_proc_stat(pid_t pid, ProcessInfo& info);
    void read_proc_status(pid_t pid, ProcessInfo& info);
    void calculate_cpu_percent();
    void apply_sort_and_filter();

    std::vector<ProcessInfo> processes_;
    std::vector<ProcessInfo> prev_processes_;
    std::vector<ProcessInfo> filtered_processes_;

    ProcessSort sort_by_ = ProcessSort::Cpu;
    bool sort_ascending_ = false;
    std::string filter_;

    uint64_t total_cpu_time_ = 0;
    uint64_t prev_total_cpu_time_ = 0;
    long page_size_ = 4096;
    long clock_ticks_ = 100;
    uint64_t total_memory_kb_ = 0;
    bool first_sample_ = true;

    // UI state
    int selected_pid_ = -1;
};

} // namespace straylight::sysmon
