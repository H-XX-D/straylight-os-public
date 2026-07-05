// tools/dash/widgets.h
// Data collection widgets for the StrayLight terminal dashboard.
#pragma once

#include "tui.h"

#include <cstdint>
#include <string>
#include <vector>

namespace straylight {

/// CPU information for one core.
struct CpuInfo {
    int id = 0;
    double usage = 0.0;     // 0-100%
    double freq_mhz = 0.0;
};

/// Memory breakdown.
struct MemInfo {
    size_t total_kb = 0;
    size_t used_kb = 0;
    size_t buffers_kb = 0;
    size_t cached_kb = 0;
    size_t free_kb = 0;
    size_t swap_total_kb = 0;
    size_t swap_used_kb = 0;
};

/// GPU information.
struct GpuInfo {
    int id = 0;
    std::string name;
    double temp_c = 0.0;
    double utilization = 0.0; // 0-100%
    size_t vram_used_mb = 0;
    size_t vram_total_mb = 0;
};

/// Disk mount info.
struct DiskInfo {
    std::string mount;
    std::string device;
    size_t total_gb = 0;
    size_t used_gb = 0;
    double usage = 0.0; // 0-100%
};

/// Network interface stats.
struct NetInfo {
    std::string iface;
    double rx_bytes_sec = 0.0;
    double tx_bytes_sec = 0.0;
};

/// Process info.
struct ProcInfo {
    int pid = 0;
    std::string user;
    std::string command;
    double cpu = 0.0;
    double mem = 0.0;
};

/// StrayLight service status.
struct ServiceStatus {
    std::string name;
    std::string state; // "running", "stopped", "failed"
};

/// Collects system data and renders dashboard widgets.
class Widgets {
public:
    Widgets();
    ~Widgets();

    /// Collect all system data.
    void refresh();

    /// Render the CPU panel.
    void render_cpu(TUI& tui, const Rect& area);

    /// Render the memory panel.
    void render_memory(TUI& tui, const Rect& area);

    /// Render the GPU panel.
    void render_gpu(TUI& tui, const Rect& area);

    /// Render the disk panel.
    void render_disk(TUI& tui, const Rect& area);

    /// Render the network panel.
    void render_network(TUI& tui, const Rect& area);

    /// Render the process list panel.
    void render_processes(TUI& tui, const Rect& area, int sort_col = 0);

    /// Render the StrayLight services panel.
    void render_services(TUI& tui, const Rect& area);

    /// Render the Alice summary panel.
    void render_alice(TUI& tui, const Rect& area);

    /// Get the process list (for kill support).
    const std::vector<ProcInfo>& processes() const { return procs_; }

private:
    std::vector<CpuInfo> cpus_;
    MemInfo mem_;
    std::vector<GpuInfo> gpus_;
    std::vector<DiskInfo> disks_;
    std::vector<NetInfo> nets_;
    std::vector<ProcInfo> procs_;
    std::vector<ServiceStatus> services_;
    std::string alice_summary_;

    // Network history for sparklines.
    std::vector<std::vector<double>> rx_history_;
    std::vector<std::vector<double>> tx_history_;
    static constexpr int kHistoryLen = 60;

    // Previous CPU sample for delta calculation.
    struct CpuSample {
        long long user = 0, nice = 0, system = 0, idle = 0,
                  iowait = 0, irq = 0, softirq = 0, steal = 0;
    };
    std::vector<CpuSample> prev_cpu_;

    // Previous network bytes for throughput calculation.
    struct NetSample {
        long long rx_bytes = 0, tx_bytes = 0;
    };
    std::vector<NetSample> prev_net_;

    void collect_cpu();
    void collect_memory();
    void collect_gpu();
    void collect_disk();
    void collect_network();
    void collect_processes();
    void collect_services();
    void collect_alice();

    std::string run_cmd(const std::string& cmd);
    std::string human_bytes(double bytes);
};

} // namespace straylight
