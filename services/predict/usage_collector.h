/**
 * StrayLight Usage Collector — Gathers current system state for the predictor.
 *
 * Collects:
 *   - Running processes (from /proc or ps)
 *   - Open files per process
 *   - GPU/VPU allocations (from sysfs)
 *   - System resource utilization
 *
 * This data feeds back into the prediction model and helps the preloader
 * make informed decisions about which resources to pre-warm.
 */
#pragma once

#include "straylight/result.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

namespace straylight::predict {

// ─── Types ──────────────────────────────────────────────────────────────────

struct ProcessInfo {
    int pid = 0;
    std::string name;
    std::string cmdline;
    double cpu_percent = 0.0;
    size_t rss_bytes = 0;
    std::vector<std::string> open_files;
};

struct VpuAllocation {
    std::string client_name;
    size_t allocated_bytes = 0;
    double utilization = 0.0;
};

struct SystemSnapshot {
    std::chrono::system_clock::time_point timestamp;
    std::vector<ProcessInfo> processes;
    std::vector<VpuAllocation> vpu_allocations;
    size_t total_ram_bytes = 0;
    size_t free_ram_bytes = 0;
    size_t total_vram_bytes = 0;
    size_t free_vram_bytes = 0;
    double cpu_load_1m = 0.0;
    double cpu_load_5m = 0.0;
    double cpu_load_15m = 0.0;
};

// ─── Usage Collector ────────────────────────────────────────────────────────

class UsageCollector {
public:
    UsageCollector() = default;

    /** Collect a full system snapshot. */
    Result<SystemSnapshot, std::string> collect() {
        std::lock_guard<std::mutex> lock(mtx_);

        SystemSnapshot snap;
        snap.timestamp = std::chrono::system_clock::now();

        // Collect running processes
        snap.processes = collect_processes();

        // Collect VPU allocations
        snap.vpu_allocations = collect_vpu_allocations();

        // Collect memory info
        collect_memory_info(snap);

        // Collect CPU load
        collect_load_average(snap);

        last_snapshot_ = snap;
        return Result<SystemSnapshot, std::string>::ok(std::move(snap));
    }

    /** Get just the list of running app names (deduped). */
    std::vector<std::string> running_apps() const {
        std::set<std::string> apps;

        std::string output = exec_cmd(
            "ps -eo comm --no-headers 2>/dev/null | sort -u");

        size_t pos = 0;
        while (pos < output.size()) {
            auto nl = output.find('\n', pos);
            if (nl == std::string::npos) nl = output.size();
            std::string name = output.substr(pos, nl - pos);
            pos = nl + 1;

            // Trim whitespace
            while (!name.empty() && (name.back() == ' ' || name.back() == '\n'))
                name.pop_back();
            auto start = name.find_first_not_of(" \t");
            if (start != std::string::npos) {
                name = name.substr(start);
            }

            // Filter out kernel threads and common system processes
            if (!name.empty() && name[0] != '[' &&
                name != "ps" && name != "sort") {
                apps.insert(name);
            }
        }

        return std::vector<std::string>(apps.begin(), apps.end());
    }

    /** Get the currently focused/foreground app. */
    std::string focused_app() const {
        // Try sway/i3 IPC
        std::string output = exec_cmd(
            "swaymsg -t get_tree 2>/dev/null | "
            "grep -oP '\"app_id\":\\s*\"\\K[^\"]+' | tail -1");

        if (output.empty()) {
            // Fallback: try xdotool
            output = exec_cmd(
                "xdotool getactivewindow getwindowpid 2>/dev/null | "
                "xargs -I{} ps -p {} -o comm= 2>/dev/null");
        }

        // Trim
        while (!output.empty() && (output.back() == '\n' || output.back() == '\r'))
            output.pop_back();

        return output;
    }

    /** Get the last collected snapshot (without re-collecting). */
    SystemSnapshot last_snapshot() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return last_snapshot_;
    }

private:
    mutable std::mutex mtx_;
    SystemSnapshot last_snapshot_;

    std::vector<ProcessInfo> collect_processes() {
        std::vector<ProcessInfo> procs;

        // Use ps with custom format for cross-platform compatibility
        std::string output = exec_cmd(
            "ps -eo pid,pcpu,rss,comm --no-headers --sort=-pcpu 2>/dev/null | head -50");

        size_t pos = 0;
        while (pos < output.size()) {
            auto nl = output.find('\n', pos);
            if (nl == std::string::npos) nl = output.size();
            std::string line = output.substr(pos, nl - pos);
            pos = nl + 1;

            if (line.empty()) continue;

            ProcessInfo proc;

            // Parse: "  PID  %CPU   RSS COMMAND"
            auto start = line.find_first_not_of(" \t");
            if (start == std::string::npos) continue;

            // PID
            auto space1 = line.find_first_of(" \t", start);
            if (space1 == std::string::npos) continue;
            try { proc.pid = std::stoi(line.substr(start, space1 - start)); }
            catch (...) { continue; }

            // CPU%
            auto cpu_start = line.find_first_not_of(" \t", space1);
            if (cpu_start == std::string::npos) continue;
            auto space2 = line.find_first_of(" \t", cpu_start);
            if (space2 == std::string::npos) continue;
            try { proc.cpu_percent = std::stod(line.substr(cpu_start, space2 - cpu_start)); }
            catch (...) { continue; }

            // RSS (in KB)
            auto rss_start = line.find_first_not_of(" \t", space2);
            if (rss_start == std::string::npos) continue;
            auto space3 = line.find_first_of(" \t", rss_start);
            if (space3 == std::string::npos) continue;
            try { proc.rss_bytes = std::stoul(line.substr(rss_start, space3 - rss_start)) * 1024; }
            catch (...) { continue; }

            // Command name
            auto name_start = line.find_first_not_of(" \t", space3);
            if (name_start == std::string::npos) continue;
            proc.name = line.substr(name_start);
            while (!proc.name.empty() && (proc.name.back() == '\n' || proc.name.back() == ' '))
                proc.name.pop_back();

            if (proc.name.empty()) continue;

            // Get open files for top processes (expensive, limit to top 10)
            if (procs.size() < 10) {
                proc.open_files = get_open_files(proc.pid);
            }

            procs.push_back(proc);
        }

        return procs;
    }

    std::vector<std::string> get_open_files(int pid) {
        std::vector<std::string> files;

        // Read /proc/PID/fd symlinks
        namespace fs = std::filesystem;
        std::string fd_dir = "/proc/" + std::to_string(pid) + "/fd";
        std::error_code ec;

        if (!fs::is_directory(fd_dir, ec)) return files;

        for (const auto& entry : fs::directory_iterator(fd_dir, ec)) {
            char link_target[1024];
            ssize_t len = readlink(entry.path().c_str(), link_target, sizeof(link_target) - 1);
            if (len > 0) {
                link_target[len] = '\0';
                std::string target(link_target);
                // Only include regular files, not sockets/pipes
                if (!target.empty() && target[0] == '/' &&
                    target.find("socket:") == std::string::npos &&
                    target.find("pipe:") == std::string::npos &&
                    target.find("/dev/") == std::string::npos &&
                    target.find("/proc/") == std::string::npos &&
                    target.find("/sys/") == std::string::npos) {
                    files.push_back(target);
                }
            }
        }

        // Deduplicate
        std::sort(files.begin(), files.end());
        files.erase(std::unique(files.begin(), files.end()), files.end());

        return files;
    }

    std::vector<VpuAllocation> collect_vpu_allocations() {
        std::vector<VpuAllocation> allocs;

        // Future/class ABI: per-client VPU allocation list.
        std::ifstream f("/sys/class/straylight-vpu/vpu0/clients");
        if (f) {
            std::string line;
            while (std::getline(f, line)) {
                if (line.empty()) continue;

                // Expected format: "client_name allocated_bytes utilization%"
                VpuAllocation alloc;
                auto space1 = line.find(' ');
                if (space1 == std::string::npos) continue;

                alloc.client_name = line.substr(0, space1);

                auto num_start = line.find_first_not_of(" \t", space1);
                if (num_start == std::string::npos) continue;

                auto space2 = line.find(' ', num_start);
                try {
                    alloc.allocated_bytes = std::stoul(line.substr(num_start,
                        space2 == std::string::npos ? std::string::npos : space2 - num_start));
                } catch (...) { continue; }

                if (space2 != std::string::npos) {
                    auto util_start = line.find_first_not_of(" \t", space2);
                    if (util_start != std::string::npos) {
                        try { alloc.utilization = std::stod(line.substr(util_start)); }
                        catch (...) {}
                    }
                }

                allocs.push_back(alloc);
            }
            return allocs;
        }

        // Live Z6 ABI: aggregate VPU kernel state.
        size_t used = 0;
        size_t total = 0;
        {
            std::ifstream vf("/sys/kernel/straylight-vpu/gpu0/vram_used");
            if (vf) vf >> used;
        }
        {
            std::ifstream vf("/sys/kernel/straylight-vpu/gpu0/vram_total");
            if (vf) vf >> total;
        }
        if (total > 0 || used > 0) {
            VpuAllocation alloc;
            alloc.client_name = "straylight-vpu";
            alloc.allocated_bytes = used;
            alloc.utilization = total > 0
                ? (static_cast<double>(used) / static_cast<double>(total)) * 100.0
                : 0.0;
            allocs.push_back(alloc);
        }

        return allocs;
    }

    void collect_memory_info(SystemSnapshot& snap) {
        std::ifstream f("/proc/meminfo");
        if (!f) return;

        std::string line;
        while (std::getline(f, line)) {
            auto parse_kb = [&](const std::string& prefix, size_t& target) {
                if (line.find(prefix) == 0) {
                    auto colon = line.find(':');
                    if (colon != std::string::npos) {
                        try { target = std::stoul(line.substr(colon + 1)) * 1024; }
                        catch (...) {}
                    }
                }
            };

            parse_kb("MemTotal:", snap.total_ram_bytes);
            parse_kb("MemAvailable:", snap.free_ram_bytes);
        }

        auto read_size = [](const char* path, size_t& out) {
            std::ifstream vf(path);
            if (vf) vf >> out;
            return static_cast<bool>(vf);
        };

        // Live VPU memory ABI on Z6.
        if (read_size("/sys/kernel/straylight-vpu/gpu0/vram_total", snap.total_vram_bytes)) {
            size_t used = 0;
            read_size("/sys/kernel/straylight-vpu/gpu0/vram_used", used);
            snap.free_vram_bytes = snap.total_vram_bytes > used
                ? snap.total_vram_bytes - used
                : 0;
            return;
        }

        // Older class ABI fallback.
        {
            std::ifstream vf("/sys/class/straylight-vpu/vpu0/memory_total");
            if (vf) vf >> snap.total_vram_bytes;
        }
        {
            std::ifstream vf("/sys/class/straylight-vpu/vpu0/memory_free");
            if (vf) vf >> snap.free_vram_bytes;
        }
    }

    void collect_load_average(SystemSnapshot& snap) {
        std::ifstream f("/proc/loadavg");
        if (f) {
            f >> snap.cpu_load_1m >> snap.cpu_load_5m >> snap.cpu_load_15m;
        }
    }

    static std::string exec_cmd(const std::string& cmd) {
        std::string result;
        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) return "";
        char buf[4096];
        while (fgets(buf, sizeof(buf), pipe) != nullptr) {
            result += buf;
        }
        pclose(pipe);
        return result;
    }
};

} // namespace straylight::predict
