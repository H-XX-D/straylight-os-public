// tools/perf/main.cpp
// CLI front-end for straylight-perf — performance counters.

#include "perf_counter.h"

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>

// ---------------------------------------------------------------------------
// Usage
// ---------------------------------------------------------------------------

static void print_usage() {
    std::cerr
        << "straylight-perf — performance counter tool\n"
        << "\n"
        << "Usage:\n"
        << "  straylight-perf summary                             Full system overview\n"
        << "  straylight-perf cpu                                  CPU metrics\n"
        << "  straylight-perf cpu --per-core                       Per-core CPU metrics\n"
        << "  straylight-perf memory                               Memory metrics\n"
        << "  straylight-perf disk                                 Disk I/O metrics\n"
        << "  straylight-perf net                                  Network I/O metrics\n"
        << "  straylight-perf thermal                              Thermal sensors\n"
        << "  straylight-perf top [--count=N] [--sort=cpu|mem]     Top processes\n";
}

// ---------------------------------------------------------------------------
// Argument helpers
// ---------------------------------------------------------------------------

static std::string get_arg(int argc, char* argv[],
                            const std::string& prefix, int start = 2) {
    for (int i = start; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind(prefix, 0) == 0) return arg.substr(prefix.size());
    }
    return "";
}

static bool has_flag(int argc, char* argv[],
                     const std::string& flag, int start = 2) {
    for (int i = start; i < argc; ++i)
        if (std::string(argv[i]) == flag) return true;
    return false;
}

static std::string pad(const std::string& s, size_t width) {
    if (s.size() >= width) return s;
    return s + std::string(width - s.size(), ' ');
}

static std::string human_bytes(uint64_t bytes) {
    const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    int idx = 0;
    double val = static_cast<double>(bytes);
    while (val >= 1024.0 && idx < 4) { val /= 1024.0; ++idx; }
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f %s", val, units[idx]);
    return buf;
}

// ===========================================================================
// main
// ===========================================================================

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string command = argv[1];
    if (command == "--help" || command == "-h") {
        print_usage();
        return 0;
    }

    straylight::PerfCounter perf;

    // -----------------------------------------------------------------------
    // summary
    // -----------------------------------------------------------------------
    if (command == "summary") {
        auto res = perf.summary();
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << res.value();
        return 0;
    }

    // -----------------------------------------------------------------------
    // cpu [--per-core]
    // -----------------------------------------------------------------------
    if (command == "cpu") {
        if (has_flag(argc, argv, "--per-core")) {
            auto res = perf.cpu_per_core();
            if (!res.has_value()) {
                std::cerr << "Error: " << res.error() << "\n";
                return 1;
            }
            std::cout << pad("CORE", 8) << pad("USER%", 10) << pad("SYS%", 10)
                      << pad("IDLE%", 10) << pad("IOWAIT%", 10)
                      << pad("IRQ%", 10) << "SOFTIRQ%\n"
                      << std::string(68, '-') << "\n";
            for (const auto& c : res.value()) {
                char line[128];
                snprintf(line, sizeof(line), "%-8d%-10.1f%-10.1f%-10.1f%-10.1f%-10.1f%.1f\n",
                         c.cpu_id, c.user_percent, c.system_percent,
                         c.idle_percent, c.iowait_percent,
                         c.irq_percent, c.softirq_percent);
                std::cout << line;
            }
        } else {
            auto res = perf.cpu();
            if (!res.has_value()) {
                std::cerr << "Error: " << res.error() << "\n";
                return 1;
            }
            const auto& c = res.value();
            std::cout << "CPU Metrics (aggregate):\n"
                      << "  User:       " << std::fixed << std::setprecision(1) << c.user_percent << "%\n"
                      << "  System:     " << c.system_percent << "%\n"
                      << "  Idle:       " << c.idle_percent << "%\n"
                      << "  IO Wait:    " << c.iowait_percent << "%\n"
                      << "  IRQ:        " << c.irq_percent << "%\n"
                      << "  Soft IRQ:   " << c.softirq_percent << "%\n"
                      << "  Steal:      " << c.steal_percent << "%\n"
                      << "\n"
                      << "  Context Switches: " << c.context_switches << "\n"
                      << "  Interrupts:       " << c.interrupts << "\n"
                      << "  Load Average:     " << std::setprecision(2)
                      << c.load_1 << " " << c.load_5 << " " << c.load_15 << "\n"
                      << "  Running Procs:    " << c.running_procs << "\n";
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // memory
    // -----------------------------------------------------------------------
    if (command == "memory") {
        auto res = perf.memory();
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        const auto& m = res.value();
        double used_pct = (m.total > 0) ? 100.0 * m.used / m.total : 0;
        std::cout << "Memory Metrics:\n"
                  << "  Total:       " << human_bytes(m.total) << "\n"
                  << "  Used:        " << human_bytes(m.used) << " ("
                  << std::fixed << std::setprecision(1) << used_pct << "%)\n"
                  << "  Free:        " << human_bytes(m.free) << "\n"
                  << "  Available:   " << human_bytes(m.available) << "\n"
                  << "  Buffers:     " << human_bytes(m.buffers) << "\n"
                  << "  Cached:      " << human_bytes(m.cached) << "\n"
                  << "  Slab:        " << human_bytes(m.slab) << "\n"
                  << "  Page Tables: " << human_bytes(m.page_tables) << "\n"
                  << "  Dirty:       " << human_bytes(m.dirty) << "\n"
                  << "  Writeback:   " << human_bytes(m.writeback) << "\n";
        if (m.swap_total > 0) {
            double swap_pct = 100.0 * m.swap_used / m.swap_total;
            std::cout << "\n  Swap Total:  " << human_bytes(m.swap_total) << "\n"
                      << "  Swap Used:   " << human_bytes(m.swap_used) << " ("
                      << swap_pct << "%)\n"
                      << "  Swap Free:   " << human_bytes(m.swap_free) << "\n";
        }
        if (m.huge_pages_total > 0) {
            std::cout << "\n  Huge Pages:  " << m.huge_pages_free << " / "
                      << m.huge_pages_total << " free\n"
                      << "  Page Size:   " << human_bytes(m.huge_page_size) << "\n";
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // disk
    // -----------------------------------------------------------------------
    if (command == "disk") {
        auto res = perf.disk();
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        const auto& disks = res.value();
        if (disks.empty()) {
            std::cout << "No disk I/O data available.\n";
            return 0;
        }
        std::cout << pad("DEVICE", 12) << pad("READS", 10) << pad("WRITES", 10)
                  << pad("READ", 12) << pad("WRITTEN", 12)
                  << pad("RD TIME", 10) << pad("WR TIME", 10) << "IO Q\n"
                  << std::string(86, '-') << "\n";
        for (const auto& d : disks) {
            std::cout << pad(d.device, 12)
                      << pad(std::to_string(d.reads_completed), 10)
                      << pad(std::to_string(d.writes_completed), 10)
                      << pad(human_bytes(d.read_bytes), 12)
                      << pad(human_bytes(d.write_bytes), 12);
            char time_buf[16];
            snprintf(time_buf, sizeof(time_buf), "%.0fms", d.read_time_ms);
            std::cout << pad(time_buf, 10);
            snprintf(time_buf, sizeof(time_buf), "%.0fms", d.write_time_ms);
            std::cout << pad(time_buf, 10)
                      << d.io_in_progress << "\n";
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // net
    // -----------------------------------------------------------------------
    if (command == "net") {
        auto res = perf.net();
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        const auto& ifaces = res.value();
        if (ifaces.empty()) {
            std::cout << "No network interfaces found.\n";
            return 0;
        }
        std::cout << pad("INTERFACE", 14) << pad("RX BYTES", 14) << pad("TX BYTES", 14)
                  << pad("RX PKTS", 12) << pad("TX PKTS", 12)
                  << pad("ERRORS", 10) << "DROPPED\n"
                  << std::string(86, '-') << "\n";
        for (const auto& n : ifaces) {
            std::cout << pad(n.interface, 14)
                      << pad(human_bytes(n.rx_bytes), 14)
                      << pad(human_bytes(n.tx_bytes), 14)
                      << pad(std::to_string(n.rx_packets), 12)
                      << pad(std::to_string(n.tx_packets), 12)
                      << pad(std::to_string(n.rx_errors + n.tx_errors), 10)
                      << (n.rx_dropped + n.tx_dropped) << "\n";
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // thermal
    // -----------------------------------------------------------------------
    if (command == "thermal") {
        auto res = perf.thermal();
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        const auto& temps = res.value();
        if (temps.empty()) {
            std::cout << "No thermal sensors found.\n";
            return 0;
        }
        std::cout << pad("ZONE", 20) << pad("TYPE", 20)
                  << pad("TEMP", 12) << "TRIP POINT\n"
                  << std::string(64, '-') << "\n";
        for (const auto& t : temps) {
            char temp_buf[16], trip_buf[16];
            snprintf(temp_buf, sizeof(temp_buf), "%.1f C", t.temp_celsius);
            if (t.trip_point > 0)
                snprintf(trip_buf, sizeof(trip_buf), "%.1f C", t.trip_point);
            else
                snprintf(trip_buf, sizeof(trip_buf), "-");
            std::cout << pad(t.zone, 20) << pad(t.type, 20)
                      << pad(temp_buf, 12) << trip_buf << "\n";
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // top [--count=N] [--sort=cpu|mem]
    // -----------------------------------------------------------------------
    if (command == "top") {
        int count = 20;
        std::string count_str = get_arg(argc, argv, "--count=", 2);
        if (!count_str.empty()) count = std::stoi(count_str);
        std::string sort_by = get_arg(argc, argv, "--sort=", 2);
        if (sort_by.empty()) sort_by = "cpu";

        auto res = perf.top(count, sort_by);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        const auto& procs = res.value();
        std::cout << pad("PID", 8) << pad("NAME", 20) << pad("STATE", 6)
                  << pad("CPU%", 8) << pad("RSS", 12) << pad("VIRT", 12)
                  << pad("THR", 6) << pad("VOL_CS", 10) << "INVOL_CS\n"
                  << std::string(92, '-') << "\n";
        for (const auto& p : procs) {
            char cpu_buf[16];
            snprintf(cpu_buf, sizeof(cpu_buf), "%.1f", p.cpu_percent);
            std::cout << pad(std::to_string(p.pid), 8)
                      << pad(p.name, 20)
                      << pad(p.state, 6)
                      << pad(cpu_buf, 8)
                      << pad(human_bytes(p.rss_kb * 1024), 12)
                      << pad(human_bytes(p.vsize_kb * 1024), 12)
                      << pad(std::to_string(p.threads), 6)
                      << pad(std::to_string(p.voluntary_switches), 10)
                      << p.involuntary_switches << "\n";
        }
        return 0;
    }

    std::cerr << "Error: unknown command '" << command << "'\n";
    print_usage();
    return 1;
}
