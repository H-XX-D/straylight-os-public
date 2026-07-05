// tools/perf/perf_counter.cpp
// Full performance counter implementation for StrayLight OS.

#include "perf_counter.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace straylight {

PerfCounter::PerfCounter() = default;
PerfCounter::~PerfCounter() = default;

std::string PerfCounter::read_file(const std::string& path) const {
    std::ifstream f(path);
    if (!f.is_open()) return "";
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

uint64_t PerfCounter::read_uint64_from(const std::string& path) const {
    std::string content = read_file(path);
    if (content.empty()) return 0;
    try { return std::stoull(content); }
    catch (...) { return 0; }
}

std::vector<uint64_t> PerfCounter::parse_cpu_line(const std::string& line) const {
    std::vector<uint64_t> values;
    std::istringstream ss(line);
    std::string label;
    ss >> label; // skip "cpu" or "cpuN"
    uint64_t val;
    while (ss >> val) values.push_back(val);
    return values;
}

CpuMetrics PerfCounter::compute_cpu(const std::vector<uint64_t>& v) const {
    CpuMetrics m;
    if (v.size() < 7) return m;

    // Fields: user, nice, system, idle, iowait, irq, softirq, steal, guest, guest_nice
    uint64_t user    = v[0] + v[1]; // user + nice
    uint64_t system  = v[2];
    uint64_t idle    = v[3];
    uint64_t iowait  = v[4];
    uint64_t irq     = v[5];
    uint64_t softirq = v[6];
    uint64_t steal   = (v.size() > 7) ? v[7] : 0;

    uint64_t total = user + system + idle + iowait + irq + softirq + steal;
    if (total == 0) return m;

    double t = static_cast<double>(total);
    m.user_percent    = 100.0 * user / t;
    m.system_percent  = 100.0 * system / t;
    m.idle_percent    = 100.0 * idle / t;
    m.iowait_percent  = 100.0 * iowait / t;
    m.irq_percent     = 100.0 * irq / t;
    m.softirq_percent = 100.0 * softirq / t;
    m.steal_percent   = 100.0 * steal / t;

    return m;
}

Result<CpuMetrics, std::string> PerfCounter::cpu() const {
    std::string stat = read_file("/proc/stat");
    if (stat.empty())
        return Result<CpuMetrics, std::string>::error("cannot read /proc/stat");

    std::istringstream stream(stat);
    std::string line;
    CpuMetrics metrics;

    while (std::getline(stream, line)) {
        if (line.rfind("cpu ", 0) == 0) {
            auto values = parse_cpu_line(line);
            metrics = compute_cpu(values);
            metrics.cpu_id = -1;
        }
        if (line.rfind("ctxt ", 0) == 0) {
            std::istringstream ss(line); std::string label;
            ss >> label >> metrics.context_switches;
        }
        if (line.rfind("intr ", 0) == 0) {
            std::istringstream ss(line); std::string label;
            ss >> label >> metrics.interrupts;
        }
        if (line.rfind("procs_running ", 0) == 0) {
            std::istringstream ss(line); std::string label;
            ss >> label >> metrics.running_procs;
        }
        if (line.rfind("processes ", 0) == 0) {
            std::istringstream ss(line); std::string label;
            ss >> label >> metrics.total_procs;
        }
    }

    // Load averages
    std::string loadavg = read_file("/proc/loadavg");
    if (!loadavg.empty()) {
        std::istringstream ss(loadavg);
        ss >> metrics.load_1 >> metrics.load_5 >> metrics.load_15;
    }

    return Result<CpuMetrics, std::string>::ok(metrics);
}

Result<std::vector<CpuMetrics>, std::string> PerfCounter::cpu_per_core() const {
    std::string stat = read_file("/proc/stat");
    if (stat.empty())
        return Result<std::vector<CpuMetrics>, std::string>::error("cannot read /proc/stat");

    std::vector<CpuMetrics> cores;
    std::istringstream stream(stat);
    std::string line;

    while (std::getline(stream, line)) {
        if (line.rfind("cpu", 0) == 0 && line.size() > 3 && line[3] != ' ') {
            auto values = parse_cpu_line(line);
            CpuMetrics m = compute_cpu(values);
            // Extract core ID from "cpuN"
            try { m.cpu_id = std::stoi(line.substr(3)); } catch (...) {}
            cores.push_back(m);
        }
    }

    return Result<std::vector<CpuMetrics>, std::string>::ok(cores);
}

Result<MemMetrics, std::string> PerfCounter::memory() const {
    std::string meminfo = read_file("/proc/meminfo");
    if (meminfo.empty())
        return Result<MemMetrics, std::string>::error("cannot read /proc/meminfo");

    MemMetrics m;
    std::istringstream stream(meminfo);
    std::string line;

    while (std::getline(stream, line)) {
        std::istringstream ss(line);
        std::string key;
        uint64_t val;
        ss >> key >> val;
        // Remove trailing ':'
        if (!key.empty() && key.back() == ':') key.pop_back();

        if (key == "MemTotal") m.total = val * 1024;
        else if (key == "MemFree") m.free = val * 1024;
        else if (key == "MemAvailable") m.available = val * 1024;
        else if (key == "Buffers") m.buffers = val * 1024;
        else if (key == "Cached") m.cached = val * 1024;
        else if (key == "SwapTotal") m.swap_total = val * 1024;
        else if (key == "SwapFree") m.swap_free = val * 1024;
        else if (key == "Slab") m.slab = val * 1024;
        else if (key == "PageTables") m.page_tables = val * 1024;
        else if (key == "Dirty") m.dirty = val * 1024;
        else if (key == "Writeback") m.writeback = val * 1024;
        else if (key == "HugePages_Total") m.huge_pages_total = val;
        else if (key == "HugePages_Free") m.huge_pages_free = val;
        else if (key == "Hugepagesize") m.huge_page_size = val * 1024;
    }

    m.used = m.total - m.available;
    m.swap_used = m.swap_total - m.swap_free;

    return Result<MemMetrics, std::string>::ok(m);
}

Result<std::vector<DiskMetrics>, std::string> PerfCounter::disk() const {
    std::string diskstats = read_file("/proc/diskstats");
    if (diskstats.empty())
        return Result<std::vector<DiskMetrics>, std::string>::error("cannot read /proc/diskstats");

    std::vector<DiskMetrics> disks;
    std::istringstream stream(diskstats);
    std::string line;

    while (std::getline(stream, line)) {
        std::istringstream ss(line);
        int major, minor;
        std::string dev;
        ss >> major >> minor >> dev;

        // Skip partitions (keep whole devices), loop devices, ram
        if (dev.find("loop") == 0 || dev.find("ram") == 0) continue;
        // Simple heuristic: skip if name ends with a digit and has letters before it
        // (e.g., sda1 is a partition of sda). Keep nvmeXnY but skip nvmeXnYpZ.
        bool is_partition = false;
        if (dev.find("nvme") == 0) {
            if (dev.find('p') != std::string::npos &&
                dev.rfind('p') > dev.rfind('n'))
                is_partition = true;
        } else if (!dev.empty() && std::isdigit(dev.back()) &&
                   dev.size() > 1 && std::isalpha(dev[dev.size() - 2])) {
            is_partition = true;
        }
        if (is_partition) continue;

        DiskMetrics d;
        d.device = dev;

        uint64_t rd_completed, rd_merged, rd_sectors, rd_time;
        uint64_t wr_completed, wr_merged, wr_sectors, wr_time;
        uint64_t io_cur, io_time, weighted_io_time;

        if (ss >> rd_completed >> rd_merged >> rd_sectors >> rd_time
               >> wr_completed >> wr_merged >> wr_sectors >> wr_time
               >> io_cur >> io_time >> weighted_io_time) {
            d.reads_completed = rd_completed;
            d.writes_completed = wr_completed;
            d.read_bytes = rd_sectors * 512;
            d.write_bytes = wr_sectors * 512;
            d.read_time_ms = static_cast<double>(rd_time);
            d.write_time_ms = static_cast<double>(wr_time);
            d.io_in_progress = io_cur;
            d.io_time_ms = static_cast<double>(io_time);
        }

        // Only include devices with any activity
        if (d.reads_completed > 0 || d.writes_completed > 0)
            disks.push_back(d);
    }

    return Result<std::vector<DiskMetrics>, std::string>::ok(disks);
}

Result<std::vector<NetMetrics>, std::string> PerfCounter::net() const {
    std::string netdev = read_file("/proc/net/dev");
    if (netdev.empty())
        return Result<std::vector<NetMetrics>, std::string>::error("cannot read /proc/net/dev");

    std::vector<NetMetrics> ifaces;
    std::istringstream stream(netdev);
    std::string line;
    int line_num = 0;

    while (std::getline(stream, line)) {
        if (++line_num <= 2) continue; // skip headers

        auto colon = line.find(':');
        if (colon == std::string::npos) continue;

        NetMetrics n;
        n.interface = line.substr(0, colon);
        // Trim leading whitespace
        auto start = n.interface.find_first_not_of(" \t");
        if (start != std::string::npos) n.interface = n.interface.substr(start);

        // Skip loopback
        if (n.interface == "lo") continue;

        std::istringstream ss(line.substr(colon + 1));
        uint64_t rx_errs, rx_drop, rx_fifo, rx_frame, rx_compressed, rx_multicast;
        uint64_t tx_errs, tx_drop, tx_fifo, tx_colls, tx_carrier, tx_compressed;

        ss >> n.rx_bytes >> n.rx_packets >> rx_errs >> rx_drop
           >> rx_fifo >> rx_frame >> rx_compressed >> rx_multicast
           >> n.tx_bytes >> n.tx_packets >> tx_errs >> tx_drop
           >> tx_fifo >> tx_colls >> tx_carrier >> tx_compressed;

        n.rx_errors = rx_errs;
        n.tx_errors = tx_errs;
        n.rx_dropped = rx_drop;
        n.tx_dropped = tx_drop;

        ifaces.push_back(n);
    }

    return Result<std::vector<NetMetrics>, std::string>::ok(ifaces);
}

Result<std::vector<ThermalReading>, std::string> PerfCounter::thermal() const {
    std::vector<ThermalReading> readings;
    std::string thermal_base = "/sys/class/thermal";

    if (!fs::exists(thermal_base))
        return Result<std::vector<ThermalReading>, std::string>::error("thermal sysfs not available");

    try {
        for (const auto& entry : fs::directory_iterator(thermal_base)) {
            std::string name = entry.path().filename().string();
            if (name.rfind("thermal_zone", 0) != 0) continue;

            ThermalReading r;
            r.zone = name;

            std::string type_content = read_file(entry.path().string() + "/type");
            if (!type_content.empty()) {
                type_content.erase(type_content.find_last_not_of(" \n\r") + 1);
                r.type = type_content;
            }

            uint64_t temp = read_uint64_from(entry.path().string() + "/temp");
            r.temp_celsius = static_cast<double>(temp) / 1000.0;

            // Read first trip point if available
            std::string trip = read_file(entry.path().string() + "/trip_point_0_temp");
            if (!trip.empty()) {
                try { r.trip_point = std::stod(trip) / 1000.0; } catch (...) {}
            }

            readings.push_back(r);
        }
    } catch (...) {}

    std::sort(readings.begin(), readings.end(),
              [](const auto& a, const auto& b) { return a.temp_celsius > b.temp_celsius; });

    return Result<std::vector<ThermalReading>, std::string>::ok(readings);
}

Result<std::vector<ProcPerf>, std::string> PerfCounter::top(int count,
                                                             const std::string& sort_by) const {
    std::vector<ProcPerf> procs;
    std::string proc_dir = "/proc";

    // Read system uptime for CPU calculations
    std::string uptime_str = read_file("/proc/uptime");
    double uptime = 0;
    if (!uptime_str.empty()) {
        try { uptime = std::stod(uptime_str); } catch (...) {}
    }

    // Get clock ticks per second
    long clk_tck = sysconf(_SC_CLK_TCK);
    if (clk_tck <= 0) clk_tck = 100;

    try {
        for (const auto& entry : fs::directory_iterator(proc_dir)) {
            if (!entry.is_directory()) continue;
            std::string name = entry.path().filename().string();
            // Only numeric directories (PIDs)
            bool is_pid = true;
            for (char c : name) { if (!std::isdigit(c)) { is_pid = false; break; } }
            if (!is_pid) continue;

            ProcPerf p;
            p.pid = std::stoi(name);

            // Read /proc/PID/stat
            std::string stat = read_file(entry.path().string() + "/stat");
            if (stat.empty()) continue;

            // Parse comm (between parentheses)
            auto open = stat.find('(');
            auto close = stat.rfind(')');
            if (open == std::string::npos || close == std::string::npos) continue;
            p.name = stat.substr(open + 1, close - open - 1);

            // Fields after ')'
            std::istringstream ss(stat.substr(close + 2));
            std::string state;
            int ppid, pgrp, session, tty;
            uint64_t flags, minflt, cminflt, majflt, cmajflt;
            uint64_t utime, stime, cutime, cstime;
            int64_t priority, nice;
            int64_t num_threads;
            uint64_t itrealvalue, starttime, vsize;
            int64_t rss_pages;

            ss >> state >> ppid >> pgrp >> session >> tty
               >> flags >> minflt >> cminflt >> majflt >> cmajflt
               >> utime >> stime >> cutime >> cstime
               >> priority >> nice >> num_threads
               >> itrealvalue >> starttime >> vsize >> rss_pages;

            p.state = state;
            p.threads = static_cast<int>(num_threads);
            p.vsize_kb = vsize / 1024;
            p.rss_kb = static_cast<uint64_t>(rss_pages) * 4; // Typically 4KB pages

            // CPU percent (since process start)
            double total_time = static_cast<double>(utime + stime) / clk_tck;
            double proc_uptime = uptime - (static_cast<double>(starttime) / clk_tck);
            if (proc_uptime > 0)
                p.cpu_percent = 100.0 * total_time / proc_uptime;

            // Read context switches from /proc/PID/status
            std::string status = read_file(entry.path().string() + "/status");
            std::istringstream sts(status);
            std::string sline;
            while (std::getline(sts, sline)) {
                if (sline.rfind("voluntary_ctxt_switches:", 0) == 0) {
                    std::istringstream vs(sline); std::string k; vs >> k >> p.voluntary_switches;
                }
                if (sline.rfind("nonvoluntary_ctxt_switches:", 0) == 0) {
                    std::istringstream vs(sline); std::string k; vs >> k >> p.involuntary_switches;
                }
            }

            procs.push_back(p);
        }
    } catch (...) {}

    // Sort
    if (sort_by == "mem" || sort_by == "memory") {
        std::sort(procs.begin(), procs.end(),
                  [](const auto& a, const auto& b) { return a.rss_kb > b.rss_kb; });
    } else {
        // Default: sort by CPU
        std::sort(procs.begin(), procs.end(),
                  [](const auto& a, const auto& b) { return a.cpu_percent > b.cpu_percent; });
    }

    if (count > 0 && static_cast<int>(procs.size()) > count)
        procs.resize(count);

    return Result<std::vector<ProcPerf>, std::string>::ok(procs);
}

Result<std::string, std::string> PerfCounter::summary() const {
    std::ostringstream report;

    // CPU
    auto cpu_res = cpu();
    if (cpu_res.has_value()) {
        const auto& c = cpu_res.value();
        report << "=== CPU ===\n"
               << "  User:     " << std::fixed << std::setprecision(1) << c.user_percent << "%\n"
               << "  System:   " << c.system_percent << "%\n"
               << "  Idle:     " << c.idle_percent << "%\n"
               << "  IO Wait:  " << c.iowait_percent << "%\n"
               << "  Load:     " << std::setprecision(2) << c.load_1 << " "
               << c.load_5 << " " << c.load_15 << "\n\n";
    }

    // Memory
    auto mem_res = memory();
    if (mem_res.has_value()) {
        const auto& m = mem_res.value();
        auto human = [](uint64_t bytes) -> std::string {
            const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
            int idx = 0;
            double val = static_cast<double>(bytes);
            while (val >= 1024.0 && idx < 4) { val /= 1024.0; ++idx; }
            char buf[32];
            snprintf(buf, sizeof(buf), "%.1f %s", val, units[idx]);
            return buf;
        };
        double used_pct = (m.total > 0) ? 100.0 * m.used / m.total : 0;
        report << "=== Memory ===\n"
               << "  Total:     " << human(m.total) << "\n"
               << "  Used:      " << human(m.used) << " (" << std::fixed << std::setprecision(1) << used_pct << "%)\n"
               << "  Available: " << human(m.available) << "\n"
               << "  Cache:     " << human(m.cached + m.buffers) << "\n";
        if (m.swap_total > 0) {
            double swap_pct = 100.0 * m.swap_used / m.swap_total;
            report << "  Swap:      " << human(m.swap_used) << " / " << human(m.swap_total)
                   << " (" << swap_pct << "%)\n";
        }
        report << "\n";
    }

    // Disk
    auto disk_res = disk();
    if (disk_res.has_value()) {
        const auto& disks = disk_res.value();
        if (!disks.empty()) {
            report << "=== Disk I/O ===\n";
            for (const auto& d : disks) {
                auto human = [](uint64_t bytes) -> std::string {
                    const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
                    int idx = 0;
                    double val = static_cast<double>(bytes);
                    while (val >= 1024.0 && idx < 4) { val /= 1024.0; ++idx; }
                    char buf[32];
                    snprintf(buf, sizeof(buf), "%.1f %s", val, units[idx]);
                    return buf;
                };
                report << "  " << d.device << ": "
                       << "R=" << human(d.read_bytes) << " W=" << human(d.write_bytes)
                       << " IOPS=" << (d.reads_completed + d.writes_completed) << "\n";
            }
            report << "\n";
        }
    }

    // Thermal
    auto therm_res = thermal();
    if (therm_res.has_value()) {
        const auto& temps = therm_res.value();
        if (!temps.empty()) {
            report << "=== Thermal ===\n";
            for (const auto& t : temps) {
                report << "  " << t.type << ": " << std::fixed << std::setprecision(1) << t.temp_celsius << " C";
                if (t.trip_point > 0) report << " (trip: " << t.trip_point << " C)";
                report << "\n";
            }
            report << "\n";
        }
    }

    return Result<std::string, std::string>::ok(report.str());
}

} // namespace straylight
