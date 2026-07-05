/**
 * StrayLight Rewind — Process Monitor
 *
 * Tracks which processes are being monitored for checkpointing.
 * Watches for process exit, tracks resource usage of the checkpointing
 * itself, and implements adaptive checkpoint intervals.
 */
#pragma once

#include "checkpoint_engine.h"
#include "straylight/result.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace straylight::rewind {

// ── Resource usage snapshot ─────────────────────────────────────────

struct ProcessResourceUsage {
    pid_t    pid                = 0;
    uint64_t cpu_time_ms        = 0;    // total CPU time consumed
    uint64_t resident_set_kb    = 0;    // RSS in KB
    uint64_t virtual_mem_kb     = 0;    // VSZ in KB
    uint64_t io_read_bytes      = 0;    // bytes read from disk
    uint64_t io_write_bytes     = 0;    // bytes written to disk
    uint64_t num_threads        = 0;
    std::string state;                  // R, S, D, Z, T, etc.
    std::string comm;                   // process name
};

struct CheckpointCost {
    uint64_t last_duration_ms   = 0;    // how long the last checkpoint took
    uint64_t total_cpu_ms       = 0;    // cumulative CPU spent checkpointing
    uint64_t total_bytes_saved  = 0;    // cumulative bytes stored
    uint32_t total_checkpoints  = 0;
};

// ── Adaptive interval calculator ────────────────────────────────────

struct AdaptiveConfig {
    int min_interval_s     = 5;      // never checkpoint faster than this
    int max_interval_s     = 300;    // never slower than this
    int default_interval_s = 30;
    double busy_threshold  = 0.7;    // CPU usage fraction to consider "busy"
    double idle_threshold  = 0.1;    // CPU usage fraction to consider "idle"
};

// ── Process Monitor ─────────────────────────────────────────────────

class ProcessMonitor {
public:
    explicit ProcessMonitor(CheckpointEngine& engine, AdaptiveConfig adaptive = {})
        : engine_(engine), adaptive_(std::move(adaptive)) {}

    /** Periodic tick — called from daemon main loop. */
    void tick() {
        std::lock_guard lock(mu_);

        auto tracking = engine_.get_tracking_info();
        uint64_t now = CheckpointStore::now_epoch_ms();

        for (auto& info : tracking) {
            // Check if process still alive
            bool alive = process_alive(info.pid);

            auto& state = states_[info.pid];
            state.pid = info.pid;
            state.alive = alive;
            state.last_check_ms = now;

            if (!alive) {
                // Process died — record the event
                if (!state.death_recorded) {
                    state.death_recorded = true;
                    state.death_time_ms = now;
                    fprintf(stderr, "[rewind-monitor] process %d exited\n", info.pid);
                }
                continue;
            }

            // Update resource usage
            auto usage = read_process_usage(info.pid);
            if (usage.pid != 0) {
                // Compute CPU delta for adaptive interval
                if (state.last_usage.pid != 0 && state.last_sample_ms > 0) {
                    uint64_t dt = now - state.last_sample_ms;
                    if (dt > 0) {
                        uint64_t cpu_delta = usage.cpu_time_ms - state.last_usage.cpu_time_ms;
                        state.cpu_fraction = static_cast<double>(cpu_delta) /
                                           static_cast<double>(dt);
                    }
                }
                state.last_usage = usage;
                state.last_sample_ms = now;
            }
        }
    }

    /** Compute adaptive interval for a tracked process. */
    int compute_interval(pid_t pid) const {
        std::lock_guard lock(mu_);
        auto it = states_.find(pid);
        if (it == states_.end()) return adaptive_.default_interval_s;

        auto& state = it->second;
        double cpu = state.cpu_fraction;

        // Busy process => checkpoint more frequently (state changes fast)
        if (cpu >= adaptive_.busy_threshold) {
            return adaptive_.min_interval_s;
        }
        // Idle process => checkpoint less frequently (nothing changing)
        if (cpu <= adaptive_.idle_threshold) {
            return adaptive_.max_interval_s;
        }
        // Linear interpolation between min and max
        double t = (cpu - adaptive_.idle_threshold) /
                   (adaptive_.busy_threshold - adaptive_.idle_threshold);
        int interval = adaptive_.max_interval_s -
            static_cast<int>(t * (adaptive_.max_interval_s - adaptive_.min_interval_s));
        return std::clamp(interval, adaptive_.min_interval_s, adaptive_.max_interval_s);
    }

    /** Record the cost of a checkpoint operation. */
    void record_checkpoint_cost(pid_t pid, uint64_t duration_ms, uint64_t bytes_saved) {
        std::lock_guard lock(mu_);
        auto& cost = costs_[pid];
        cost.last_duration_ms = duration_ms;
        cost.total_cpu_ms += duration_ms;
        cost.total_bytes_saved += bytes_saved;
        cost.total_checkpoints++;
    }

    /** Get resource usage for a process. */
    ProcessResourceUsage get_usage(pid_t pid) const {
        std::lock_guard lock(mu_);
        auto it = states_.find(pid);
        if (it == states_.end()) return {};
        return it->second.last_usage;
    }

    /** Get checkpoint cost stats for a process. */
    CheckpointCost get_cost(pid_t pid) const {
        std::lock_guard lock(mu_);
        auto it = costs_.find(pid);
        if (it == costs_.end()) return {};
        return it->second;
    }

    /** Check if a process is still alive. */
    bool is_alive(pid_t pid) const {
        std::lock_guard lock(mu_);
        auto it = states_.find(pid);
        if (it == states_.end()) return process_alive(pid);
        return it->second.alive;
    }

    /** Get list of processes that have died since last check. */
    std::vector<pid_t> get_dead_processes() const {
        std::lock_guard lock(mu_);
        std::vector<pid_t> dead;
        for (auto& [pid, state] : states_) {
            if (!state.alive) dead.push_back(pid);
        }
        return dead;
    }

    /** Remove monitoring state for a PID. */
    void remove(pid_t pid) {
        std::lock_guard lock(mu_);
        states_.erase(pid);
        costs_.erase(pid);
    }

    /** Resolve a process name to a PID by scanning /proc. */
    static Result<pid_t, std::string> resolve_name(const std::string& name) {
        namespace fs = std::filesystem;
        std::error_code ec;
        auto proc = fs::path("/proc");
        if (!fs::exists(proc, ec))
            return Result<pid_t, std::string>::error("/proc not available");

        for (auto& entry : fs::directory_iterator(proc, ec)) {
            if (!entry.is_directory()) continue;
            auto dirname = entry.path().filename().string();
            pid_t pid = 0;
            try { pid = std::stoi(dirname); }
            catch (...) { continue; }

            // Read /proc/PID/comm
            std::ifstream comm_file(entry.path() / "comm");
            if (!comm_file) continue;
            std::string comm;
            std::getline(comm_file, comm);
            // Trim trailing newline
            while (!comm.empty() && (comm.back() == '\n' || comm.back() == '\r'))
                comm.pop_back();

            if (comm == name) {
                return Result<pid_t, std::string>::ok(pid);
            }

            // Also check /proc/PID/cmdline
            std::ifstream cmdline_file(entry.path() / "cmdline");
            if (cmdline_file) {
                std::string cmdline;
                std::getline(cmdline_file, cmdline, '\0');
                // Extract basename
                auto slash = cmdline.rfind('/');
                auto basename = (slash != std::string::npos)
                    ? cmdline.substr(slash + 1) : cmdline;
                if (basename == name) {
                    return Result<pid_t, std::string>::ok(pid);
                }
            }
        }

        return Result<pid_t, std::string>::error(
            "no process found with name: " + name);
    }

private:
    CheckpointEngine& engine_;
    AdaptiveConfig adaptive_;
    mutable std::mutex mu_;

    struct MonitorState {
        pid_t    pid             = 0;
        bool     alive          = true;
        bool     death_recorded = false;
        uint64_t death_time_ms  = 0;
        uint64_t last_check_ms  = 0;
        uint64_t last_sample_ms = 0;
        double   cpu_fraction   = 0.0;
        ProcessResourceUsage last_usage{};
    };

    std::unordered_map<pid_t, MonitorState> states_;
    std::unordered_map<pid_t, CheckpointCost> costs_;

    // ── /proc readers ───────────────────────────────────────────────

    static bool process_alive(pid_t pid) {
        return std::filesystem::exists("/proc/" + std::to_string(pid));
    }

    static ProcessResourceUsage read_process_usage(pid_t pid) {
        ProcessResourceUsage usage;
        usage.pid = pid;

        // Read /proc/PID/stat
        auto stat_path = "/proc/" + std::to_string(pid) + "/stat";
        std::ifstream stat_file(stat_path);
        if (!stat_file) return {};

        std::string stat_line;
        std::getline(stat_file, stat_line);

        // Parse stat: pid (comm) state ppid pgrp session tty_nr tpgid flags
        //   minflt cminflt majflt cmajflt utime stime cutime cstime ...
        // Find the closing paren to skip comm (may contain spaces)
        auto paren_close = stat_line.rfind(')');
        if (paren_close == std::string::npos) return {};

        // Extract comm
        auto paren_open = stat_line.find('(');
        if (paren_open != std::string::npos && paren_close > paren_open) {
            usage.comm = stat_line.substr(paren_open + 1,
                                          paren_close - paren_open - 1);
        }

        // Fields after the closing paren
        std::istringstream iss(stat_line.substr(paren_close + 2));
        std::string state;
        uint64_t ppid, pgrp, session, tty, tpgid, flags;
        uint64_t minflt, cminflt, majflt, cmajflt;
        uint64_t utime, stime, cutime, cstime;
        uint64_t priority, nice, num_threads_val, itrealvalue, starttime;
        uint64_t vsize, rss;

        iss >> state >> ppid >> pgrp >> session >> tty >> tpgid >> flags
            >> minflt >> cminflt >> majflt >> cmajflt
            >> utime >> stime >> cutime >> cstime
            >> priority >> nice >> num_threads_val >> itrealvalue >> starttime
            >> vsize >> rss;

        usage.state = state;
        // utime and stime are in clock ticks; convert to ms assuming 100 Hz
        static const long ticks_per_sec = sysconf(_SC_CLK_TCK);
        usage.cpu_time_ms = ((utime + stime) * 1000) / static_cast<uint64_t>(ticks_per_sec);
        usage.virtual_mem_kb = vsize / 1024;
        // RSS is in pages
        static const long page_size = sysconf(_SC_PAGESIZE);
        usage.resident_set_kb = (rss * static_cast<uint64_t>(page_size)) / 1024;
        usage.num_threads = num_threads_val;

        // Read /proc/PID/io for IO stats
        auto io_path = "/proc/" + std::to_string(pid) + "/io";
        std::ifstream io_file(io_path);
        if (io_file) {
            std::string line;
            while (std::getline(io_file, line)) {
                if (line.substr(0, 11) == "read_bytes:") {
                    try { usage.io_read_bytes = std::stoull(line.substr(12)); }
                    catch (...) {}
                } else if (line.substr(0, 12) == "write_bytes:") {
                    try { usage.io_write_bytes = std::stoull(line.substr(13)); }
                    catch (...) {}
                }
            }
        }

        return usage;
    }
};

} // namespace straylight::rewind
