// tools/dash/widgets.cpp
// Full implementation of system data collection and widget rendering.

#include "widgets.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace straylight {

Widgets::Widgets() = default;
Widgets::~Widgets() = default;

std::string Widgets::run_cmd(const std::string& cmd) {
    std::array<char, 4096> buffer{};
    std::string output;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return {};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
        output += buffer.data();
    }
    pclose(pipe);
    return output;
}

std::string Widgets::human_bytes(double bytes) {
    const char* units[] = {"B", "K", "M", "G", "T"};
    int idx = 0;
    while (bytes >= 1024.0 && idx < 4) {
        bytes /= 1024.0;
        ++idx;
    }
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1) << bytes << units[idx];
    return ss.str();
}

// ---------------------------------------------------------------------------
// Data collection
// ---------------------------------------------------------------------------

void Widgets::collect_cpu() {
    std::ifstream stat("/proc/stat");
    if (!stat.is_open()) {
        // Fallback: try sysctl on macOS-like.
        cpus_.clear();
        CpuInfo ci;
        ci.id = 0;
        ci.usage = 0;
        cpus_.push_back(ci);
        return;
    }

    std::vector<CpuSample> current;
    std::string line;
    while (std::getline(stat, line)) {
        if (line.substr(0, 3) != "cpu") break;
        if (line.substr(0, 4) == "cpu ") continue; // Skip aggregate.

        CpuSample s;
        int id = 0;
        sscanf(line.c_str(), "cpu%d %lld %lld %lld %lld %lld %lld %lld %lld",
               &id, &s.user, &s.nice, &s.system, &s.idle,
               &s.iowait, &s.irq, &s.softirq, &s.steal);
        current.push_back(s);
    }

    cpus_.resize(current.size());
    for (size_t i = 0; i < current.size(); ++i) {
        cpus_[i].id = static_cast<int>(i);

        if (i < prev_cpu_.size()) {
            auto& c = current[i];
            auto& p = prev_cpu_[i];
            long long total_c = c.user + c.nice + c.system + c.idle +
                                c.iowait + c.irq + c.softirq + c.steal;
            long long total_p = p.user + p.nice + p.system + p.idle +
                                p.iowait + p.irq + p.softirq + p.steal;
            long long total_d = total_c - total_p;
            long long idle_d = (c.idle + c.iowait) - (p.idle + p.iowait);
            if (total_d > 0) {
                cpus_[i].usage = 100.0 * (1.0 - static_cast<double>(idle_d) /
                                                  static_cast<double>(total_d));
            }
        }

        // Try to get frequency.
        std::string freq_path = "/sys/devices/system/cpu/cpu" +
                                 std::to_string(i) + "/cpufreq/scaling_cur_freq";
        std::ifstream freq_file(freq_path);
        if (freq_file.is_open()) {
            long long khz = 0;
            freq_file >> khz;
            cpus_[i].freq_mhz = static_cast<double>(khz) / 1000.0;
        }
    }

    prev_cpu_ = current;
}

void Widgets::collect_memory() {
    std::ifstream meminfo("/proc/meminfo");
    if (!meminfo.is_open()) return;

    std::string line;
    while (std::getline(meminfo, line)) {
        auto extract = [&](const char* key) -> size_t {
            if (line.find(key) == 0) {
                auto colon = line.find(':');
                if (colon == std::string::npos) return 0;
                return static_cast<size_t>(std::atoll(line.c_str() + colon + 1));
            }
            return 0;
        };

        if (line.find("MemTotal:") == 0) mem_.total_kb = extract("MemTotal:");
        if (line.find("MemFree:") == 0) mem_.free_kb = extract("MemFree:");
        if (line.find("Buffers:") == 0) mem_.buffers_kb = extract("Buffers:");
        if (line.find("Cached:") == 0 && line.find("SwapCached:") == std::string::npos)
            mem_.cached_kb = extract("Cached:");
        if (line.find("SwapTotal:") == 0) mem_.swap_total_kb = extract("SwapTotal:");
        if (line.find("SwapFree:") == 0) {
            size_t swap_free = extract("SwapFree:");
            mem_.swap_used_kb = mem_.swap_total_kb - swap_free;
        }
    }

    mem_.used_kb = mem_.total_kb - mem_.free_kb - mem_.buffers_kb - mem_.cached_kb;
}

void Widgets::collect_gpu() {
    gpus_.clear();

    // Try nvidia-smi first.
    std::string nv = run_cmd(
        "nvidia-smi --query-gpu=index,name,temperature.gpu,"
        "utilization.gpu,memory.used,memory.total "
        "--format=csv,noheader,nounits 2>/dev/null");

    std::istringstream iss(nv);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.empty()) continue;
        GpuInfo g;
        // Format: 0, NVIDIA GeForce ..., 45, 23, 1024, 8192
        char name_buf[256] = {};
        sscanf(line.c_str(), "%d, %255[^,], %lf, %lf, %zu, %zu",
               &g.id, name_buf, &g.temp_c, &g.utilization,
               &g.vram_used_mb, &g.vram_total_mb);
        g.name = name_buf;
        gpus_.push_back(std::move(g));
    }

    // Try sysfs for AMD/Intel GPUs if no NVIDIA found.
    if (gpus_.empty()) {
        // Check /sys/class/drm/card0/device/
        std::ifstream hwmon("/sys/class/hwmon/hwmon0/temp1_input");
        if (hwmon.is_open()) {
            GpuInfo g;
            g.id = 0;
            g.name = "Integrated GPU";
            long millideg = 0;
            hwmon >> millideg;
            g.temp_c = static_cast<double>(millideg) / 1000.0;
            gpus_.push_back(std::move(g));
        }
    }
}

void Widgets::collect_disk() {
    disks_.clear();
    std::string out = run_cmd("df -BG --output=source,target,size,used,pcent 2>/dev/null | tail -n +2");
    std::istringstream iss(out);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.empty()) continue;
        DiskInfo d;
        char dev[256] = {}, mount[256] = {};
        int total = 0, used = 0, pct = 0;
        if (sscanf(line.c_str(), "%255s %255s %dG %dG %d%%",
                   dev, mount, &total, &used, &pct) >= 4) {
            d.device = dev;
            d.mount = mount;
            d.total_gb = static_cast<size_t>(total);
            d.used_gb = static_cast<size_t>(used);
            d.usage = static_cast<double>(pct);
            // Skip pseudo-filesystems.
            if (d.device[0] == '/' || d.device.find("tmpfs") != std::string::npos) {
                disks_.push_back(std::move(d));
            }
        }
    }
}

void Widgets::collect_network() {
    std::ifstream net_dev("/proc/net/dev");
    if (!net_dev.is_open()) return;

    std::vector<NetSample> current_samples;
    std::vector<std::string> iface_names;

    std::string line;
    int lineno = 0;
    while (std::getline(net_dev, line)) {
        ++lineno;
        if (lineno <= 2) continue; // Skip headers.

        auto colon = line.find(':');
        if (colon == std::string::npos) continue;

        std::string iface = line.substr(0, colon);
        while (!iface.empty() && iface[0] == ' ') iface.erase(0, 1);

        // Skip loopback.
        if (iface == "lo") continue;

        long long rx = 0, tx = 0;
        sscanf(line.c_str() + colon + 1,
               "%lld %*d %*d %*d %*d %*d %*d %*d %lld",
               &rx, &tx);

        iface_names.push_back(iface);
        current_samples.push_back({rx, tx});
    }

    nets_.resize(iface_names.size());
    for (size_t i = 0; i < iface_names.size(); ++i) {
        nets_[i].iface = iface_names[i];
        if (i < prev_net_.size()) {
            nets_[i].rx_bytes_sec = static_cast<double>(
                current_samples[i].rx_bytes - prev_net_[i].rx_bytes);
            nets_[i].tx_bytes_sec = static_cast<double>(
                current_samples[i].tx_bytes - prev_net_[i].tx_bytes);
        }
    }
    prev_net_ = current_samples;

    // Update history for sparklines.
    rx_history_.resize(nets_.size());
    tx_history_.resize(nets_.size());
    for (size_t i = 0; i < nets_.size(); ++i) {
        rx_history_[i].push_back(nets_[i].rx_bytes_sec);
        tx_history_[i].push_back(nets_[i].tx_bytes_sec);
        if (rx_history_[i].size() > static_cast<size_t>(kHistoryLen)) {
            rx_history_[i].erase(rx_history_[i].begin());
        }
        if (tx_history_[i].size() > static_cast<size_t>(kHistoryLen)) {
            tx_history_[i].erase(tx_history_[i].begin());
        }
    }
}

void Widgets::collect_processes() {
    procs_.clear();
    std::string out = run_cmd(
        "ps -eo pid,user,%cpu,%mem,comm --sort=-%cpu 2>/dev/null | head -30");
    std::istringstream iss(out);
    std::string line;
    bool header = true;
    while (std::getline(iss, line)) {
        if (header) { header = false; continue; }
        if (line.empty()) continue;

        ProcInfo p;
        char user[64] = {}, cmd[256] = {};
        if (sscanf(line.c_str(), "%d %63s %lf %lf %255s",
                   &p.pid, user, &p.cpu, &p.mem, cmd) >= 5) {
            p.user = user;
            p.command = cmd;
            procs_.push_back(std::move(p));
        }
    }
}

void Widgets::collect_services() {
    services_.clear();
    std::vector<std::string> svc_names = {
        "straylight-alice", "straylight-whisper", "straylight-remote",
        "straylight-swarm", "straylight-autotune", "straylight-flux",
        "straylight-health", "straylight-mesh",
        "straylight-cron", "straylight-replay"
    };

    for (const auto& name : svc_names) {
        ServiceStatus s;
        s.name = name;
        std::string out = run_cmd(
            "systemctl is-active " + name + " 2>/dev/null");
        while (!out.empty() && out.back() == '\n') out.pop_back();
        s.state = out.empty() ? "unknown" : out;
        services_.push_back(std::move(s));
    }
}

void Widgets::collect_alice() {
    // Try to get Alice's last assessment via IPC or log.
    std::string out = run_cmd(
        "journalctl -u straylight-alice --since '5 minutes ago' "
        "--no-pager -o cat 2>/dev/null | tail -5");
    if (out.empty()) {
        alice_summary_ = "No recent assessments";
    } else {
        alice_summary_ = out;
    }
}

void Widgets::refresh() {
    collect_cpu();
    collect_memory();
    collect_gpu();
    collect_disk();
    collect_network();
    collect_processes();
    collect_services();
    collect_alice();
}

// ---------------------------------------------------------------------------
// Widget rendering
// ---------------------------------------------------------------------------

void Widgets::render_cpu(TUI& tui, const Rect& area) {
    tui.box(area, "CPU");

    int y = area.y + 1;
    int max_rows = area.h - 2;
    int bar_w = area.w - 18; // Space for "C00 [bar] 100% 3.5G"

    for (size_t i = 0; i < cpus_.size() && static_cast<int>(i) < max_rows; ++i) {
        const auto& c = cpus_[i];
        std::ostringstream label;
        label << "C" << std::setw(2) << std::setfill('0') << c.id << " ";
        tui.print_at(area.x + 1, y, label.str());

        // Color based on usage.
        std::string color = Color::Green;
        if (c.usage > 80) color = Color::Red;
        else if (c.usage > 50) color = Color::Yellow;

        tui.bar(area.x + 5, y, std::max(1, bar_w), c.usage / 100.0, color);

        std::ostringstream info;
        info << " " << std::fixed << std::setprecision(0) << std::setw(3) << c.usage << "%";
        if (c.freq_mhz > 0) {
            info << " " << std::setprecision(1) << c.freq_mhz / 1000.0 << "G";
        }
        tui.print(info.str());
        ++y;
    }
}

void Widgets::render_memory(TUI& tui, const Rect& area) {
    tui.box(area, "Memory");

    int y = area.y + 1;
    int bar_w = area.w - 6;

    double total = static_cast<double>(mem_.total_kb);
    if (total <= 0) total = 1;

    // RAM bar.
    tui.print_at(area.x + 1, y, "RAM");
    double usage = static_cast<double>(mem_.used_kb) / total;
    std::string color = usage > 0.9 ? Color::Red : (usage > 0.7 ? Color::Yellow : Color::Green);
    tui.bar(area.x + 5, y, std::max(1, bar_w), usage, color);
    ++y;

    // Breakdown.
    auto fmt_kb = [](size_t kb) {
        std::ostringstream ss;
        if (kb > 1048576) ss << std::fixed << std::setprecision(1) << kb / 1048576.0 << "G";
        else if (kb > 1024) ss << std::fixed << std::setprecision(0) << kb / 1024.0 << "M";
        else ss << kb << "K";
        return ss.str();
    };

    std::ostringstream detail;
    detail << " Used: " << fmt_kb(mem_.used_kb)
           << "  Buf: " << fmt_kb(mem_.buffers_kb)
           << "  Cache: " << fmt_kb(mem_.cached_kb)
           << "  Free: " << fmt_kb(mem_.free_kb)
           << "  Total: " << fmt_kb(mem_.total_kb);
    tui.print_at(area.x + 2, y, detail.str());
    ++y;

    // Swap bar.
    if (mem_.swap_total_kb > 0) {
        tui.print_at(area.x + 1, y, "SWP");
        double swap_usage = static_cast<double>(mem_.swap_used_kb) /
                            static_cast<double>(mem_.swap_total_kb);
        tui.bar(area.x + 5, y, std::max(1, bar_w), swap_usage, Color::Magenta);
        ++y;
        std::ostringstream swap_detail;
        swap_detail << " Swap: " << fmt_kb(mem_.swap_used_kb)
                    << " / " << fmt_kb(mem_.swap_total_kb);
        tui.print_at(area.x + 2, y, swap_detail.str());
    }
}

void Widgets::render_gpu(TUI& tui, const Rect& area) {
    tui.box(area, "GPU");

    int y = area.y + 1;
    int bar_w = area.w - 30;

    if (gpus_.empty()) {
        tui.print_at(area.x + 2, y, std::string(Color::Dim) + "No GPU detected" + Color::Reset);
        return;
    }

    for (const auto& g : gpus_) {
        std::ostringstream label;
        label << "GPU" << g.id << " " << g.name.substr(0, 20);
        tui.print_at(area.x + 1, y, label.str());
        ++y;

        // Temperature.
        std::string temp_color = g.temp_c > 80 ? Color::Red :
                                  (g.temp_c > 60 ? Color::Yellow : Color::Green);
        std::ostringstream temp;
        temp << " Temp: " << temp_color << std::fixed << std::setprecision(0)
             << g.temp_c << "C" << Color::Reset;
        tui.print_at(area.x + 2, y, temp.str());

        // Utilization bar.
        std::ostringstream util_label;
        util_label << "  Util: ";
        tui.print(util_label.str());
        tui.bar(area.x + 16, y, std::max(1, bar_w), g.utilization / 100.0,
                 g.utilization > 80 ? Color::Red : Color::Cyan);

        std::ostringstream pct;
        pct << " " << std::fixed << std::setprecision(0) << g.utilization << "%";
        tui.print(pct.str());
        ++y;

        // VRAM.
        if (g.vram_total_mb > 0) {
            double vram_frac = static_cast<double>(g.vram_used_mb) /
                               static_cast<double>(g.vram_total_mb);
            tui.print_at(area.x + 2, y, " VRAM: ");
            tui.bar(area.x + 9, y, std::max(1, bar_w + 7), vram_frac, Color::Blue);
            std::ostringstream vram;
            vram << " " << g.vram_used_mb << "/" << g.vram_total_mb << "M";
            tui.print(vram.str());
            ++y;
        }
    }
}

void Widgets::render_disk(TUI& tui, const Rect& area) {
    tui.box(area, "Disk");

    int y = area.y + 1;
    int bar_w = area.w - 30;

    for (const auto& d : disks_) {
        if (y >= area.y + area.h - 1) break;

        std::ostringstream label;
        label << std::left << std::setw(12) << d.mount.substr(0, 12);
        tui.print_at(area.x + 1, y, label.str());

        std::string color = d.usage > 90 ? Color::Red :
                             (d.usage > 70 ? Color::Yellow : Color::Green);
        tui.bar(area.x + 14, y, std::max(1, bar_w), d.usage / 100.0, color);

        std::ostringstream info;
        info << " " << d.used_gb << "/" << d.total_gb << "G "
             << std::fixed << std::setprecision(0) << d.usage << "%";
        tui.print(info.str());
        ++y;
    }
}

void Widgets::render_network(TUI& tui, const Rect& area) {
    tui.box(area, "Network");

    int y = area.y + 1;
    int spark_w = area.w - 30;

    for (size_t i = 0; i < nets_.size(); ++i) {
        if (y + 2 >= area.y + area.h - 1) break;
        const auto& n = nets_[i];

        tui.print_at(area.x + 1, y,
                     std::string(Color::Bold) + n.iface + Color::Reset);
        ++y;

        // RX sparkline.
        tui.print_at(area.x + 2, y, "RX ");
        if (i < rx_history_.size()) {
            tui.sparkline(area.x + 5, y, std::max(1, spark_w),
                           rx_history_[i], Color::Green);
        }
        tui.print(" " + human_bytes(n.rx_bytes_sec) + "/s");
        ++y;

        // TX sparkline.
        tui.print_at(area.x + 2, y, "TX ");
        if (i < tx_history_.size()) {
            tui.sparkline(area.x + 5, y, std::max(1, spark_w),
                           tx_history_[i], Color::Cyan);
        }
        tui.print(" " + human_bytes(n.tx_bytes_sec) + "/s");
        ++y;
    }
}

void Widgets::render_processes(TUI& tui, const Rect& area, int sort_col) {
    tui.box(area, "Processes");

    int y = area.y + 1;

    // Header.
    std::ostringstream hdr;
    hdr << std::left
        << std::setw(7) << "PID"
        << std::setw(10) << "USER"
        << std::setw(7) << "CPU%"
        << std::setw(7) << "MEM%"
        << "COMMAND";
    tui.print_at(area.x + 1, y,
                  std::string(Color::Bold) + hdr.str() + Color::Reset);
    ++y;

    // Sort by the requested column.
    auto sorted = procs_;
    if (sort_col == 1) {
        std::sort(sorted.begin(), sorted.end(),
                  [](const ProcInfo& a, const ProcInfo& b) {
                      return a.mem > b.mem;
                  });
    }
    // Default: already sorted by CPU.

    int max_rows = area.h - 3;
    for (int i = 0; i < static_cast<int>(sorted.size()) && i < max_rows; ++i) {
        const auto& p = sorted[i];
        std::ostringstream row;
        row << std::left
            << std::setw(7) << p.pid
            << std::setw(10) << p.user.substr(0, 9)
            << std::setw(7) << std::fixed << std::setprecision(1) << p.cpu
            << std::setw(7) << std::fixed << std::setprecision(1) << p.mem
            << p.command.substr(0, area.w - 35);

        // Highlight high CPU.
        if (p.cpu > 50) {
            tui.print_at(area.x + 1, y, std::string(Color::Red) + row.str() + Color::Reset);
        } else {
            tui.print_at(area.x + 1, y, row.str());
        }
        ++y;
    }
}

void Widgets::render_services(TUI& tui, const Rect& area) {
    tui.box(area, "StrayLight Services");

    int y = area.y + 1;
    for (const auto& s : services_) {
        if (y >= area.y + area.h - 1) break;

        std::string indicator;
        if (s.state == "active") {
            indicator = std::string(Color::Green) + "[*]" + Color::Reset;
        } else if (s.state == "failed") {
            indicator = std::string(Color::Red) + "[!]" + Color::Reset;
        } else if (s.state == "inactive") {
            indicator = std::string(Color::Dim) + "[-]" + Color::Reset;
        } else {
            indicator = std::string(Color::Yellow) + "[?]" + Color::Reset;
        }

        tui.print_at(area.x + 1, y, indicator + " " + s.name);
        ++y;
    }
}

void Widgets::render_alice(TUI& tui, const Rect& area) {
    tui.box(area, "Alice");

    int y = area.y + 1;
    std::istringstream iss(alice_summary_);
    std::string line;
    while (std::getline(iss, line)) {
        if (y >= area.y + area.h - 1) break;
        tui.print_at(area.x + 1, y,
                     line.substr(0, static_cast<size_t>(area.w - 2)));
        ++y;
    }
}

} // namespace straylight
