// tools/perf/perf_counter.h
// Performance counter tool for StrayLight OS.
#pragma once

#include <straylight/result.h>

#include <cstdint>
#include <string>
#include <vector>

namespace straylight {

/// CPU performance metrics.
struct CpuMetrics {
    int         cpu_id = -1;         // -1 = aggregate
    double      user_percent = 0;
    double      system_percent = 0;
    double      idle_percent = 0;
    double      iowait_percent = 0;
    double      irq_percent = 0;
    double      softirq_percent = 0;
    double      steal_percent = 0;
    uint64_t    context_switches = 0;
    uint64_t    interrupts = 0;
    double      load_1 = 0;
    double      load_5 = 0;
    double      load_15 = 0;
    int         running_procs = 0;
    int         total_procs = 0;
};

/// Memory performance metrics.
struct MemMetrics {
    uint64_t total = 0;
    uint64_t used = 0;
    uint64_t free = 0;
    uint64_t available = 0;
    uint64_t buffers = 0;
    uint64_t cached = 0;
    uint64_t swap_total = 0;
    uint64_t swap_used = 0;
    uint64_t swap_free = 0;
    uint64_t slab = 0;
    uint64_t page_tables = 0;
    uint64_t dirty = 0;
    uint64_t writeback = 0;
    uint64_t huge_pages_total = 0;
    uint64_t huge_pages_free = 0;
    uint64_t huge_page_size = 0;
};

/// Disk I/O metrics.
struct DiskMetrics {
    std::string device;
    uint64_t    reads_completed = 0;
    uint64_t    writes_completed = 0;
    uint64_t    read_bytes = 0;
    uint64_t    write_bytes = 0;
    double      read_time_ms = 0;
    double      write_time_ms = 0;
    uint64_t    io_in_progress = 0;
    double      io_time_ms = 0;
    double      utilization_percent = 0;
};

/// Network I/O metrics.
struct NetMetrics {
    std::string interface;
    uint64_t    rx_bytes = 0;
    uint64_t    tx_bytes = 0;
    uint64_t    rx_packets = 0;
    uint64_t    tx_packets = 0;
    uint64_t    rx_errors = 0;
    uint64_t    tx_errors = 0;
    uint64_t    rx_dropped = 0;
    uint64_t    tx_dropped = 0;
};

/// Thermal sensor reading.
struct ThermalReading {
    std::string zone;
    std::string type;
    double      temp_celsius = 0;
    double      trip_point = 0;
};

/// Process performance snapshot.
struct ProcPerf {
    int         pid = 0;
    std::string name;
    double      cpu_percent = 0;
    uint64_t    rss_kb = 0;
    uint64_t    vsize_kb = 0;
    int         threads = 0;
    uint64_t    voluntary_switches = 0;
    uint64_t    involuntary_switches = 0;
    std::string state;
};

class PerfCounter {
public:
    PerfCounter();
    ~PerfCounter();

    Result<CpuMetrics, std::string> cpu() const;
    Result<std::vector<CpuMetrics>, std::string> cpu_per_core() const;
    Result<MemMetrics, std::string> memory() const;
    Result<std::vector<DiskMetrics>, std::string> disk() const;
    Result<std::vector<NetMetrics>, std::string> net() const;
    Result<std::vector<ThermalReading>, std::string> thermal() const;
    Result<std::vector<ProcPerf>, std::string> top(int count, const std::string& sort_by) const;
    Result<std::string, std::string> summary() const;

private:
    uint64_t read_uint64_from(const std::string& path) const;
    std::string read_file(const std::string& path) const;
    std::vector<uint64_t> parse_cpu_line(const std::string& line) const;
    CpuMetrics compute_cpu(const std::vector<uint64_t>& values) const;
};

} // namespace straylight
