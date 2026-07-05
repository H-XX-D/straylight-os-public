/**
 * StrayLight Quota — cgroup v2 Controller
 *
 * Manages per-app cgroup hierarchies under /sys/fs/cgroup/straylight/.
 * Creates cgroups, sets resource limits, and reads usage counters.
 */
#pragma once

#include "straylight/result.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <unistd.h>
#include <unordered_map>

namespace straylight::quota {

static constexpr const char* CGROUP_ROOT = "/sys/fs/cgroup/straylight";

/** Current resource usage from cgroup controllers. */
struct CgroupUsage {
    double   cpu_percent{0.0};
    uint64_t ram_bytes{0};
    uint64_t disk_read_bytes{0};
    uint64_t disk_write_bytes{0};
    uint64_t disk_iops{0};
    uint64_t net_rx_bytes{0};
    uint64_t net_tx_bytes{0};
};

class CgroupController {
public:
    CgroupController() = default;

    /**
     * Create a cgroup for an application.
     * Creates /sys/fs/cgroup/straylight/<app>/ with appropriate controllers.
     */
    VoidResult<> create_cgroup(const std::string& app) {
        std::lock_guard<std::mutex> lock(mu_);
        namespace fs = std::filesystem;
        std::string cg_path = std::string(CGROUP_ROOT) + "/" + app;

        std::error_code ec;
        fs::create_directories(cg_path, ec);
        if (ec) {
            return VoidResult<>::error(
                "cannot create cgroup dir " + cg_path + ": " + ec.message());
        }

        // Enable controllers (cpu, memory, io) in the parent
        std::string subtree_control = std::string(CGROUP_ROOT) +
                                      "/cgroup.subtree_control";
        write_file(subtree_control, "+cpu +memory +io");

        known_cgroups_[app] = cg_path;
        return VoidResult<>::ok();
    }

    /**
     * Assign a process to an app's cgroup.
     */
    VoidResult<> assign_pid(const std::string& app, pid_t pid) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = known_cgroups_.find(app);
        std::string cg_path;
        if (it != known_cgroups_.end()) {
            cg_path = it->second;
        } else {
            cg_path = std::string(CGROUP_ROOT) + "/" + app;
        }

        std::string procs_path = cg_path + "/cgroup.procs";
        return write_file(procs_path, std::to_string(pid));
    }

    /**
     * Set CPU limit as percentage of one core (100 = 1 full core).
     * Uses cpu.max in cgroup v2: "quota period" in microseconds.
     */
    VoidResult<> set_cpu_limit(const std::string& app, double percent) {
        std::string cg_path = get_cgroup_path(app);
        uint64_t period = 100000;  // 100ms
        uint64_t quota = static_cast<uint64_t>(
            (percent / 100.0) * static_cast<double>(period));
        if (quota < 1000) quota = 1000; // minimum 1ms

        std::string value = std::to_string(quota) + " " +
                            std::to_string(period);
        return write_file(cg_path + "/cpu.max", value);
    }

    /**
     * Set memory limit in bytes.
     * Uses memory.max in cgroup v2.
     */
    VoidResult<> set_memory_limit(const std::string& app, uint64_t bytes) {
        std::string cg_path = get_cgroup_path(app);
        std::string value = (bytes == 0) ? "max" : std::to_string(bytes);
        return write_file(cg_path + "/memory.max", value);
    }

    /**
     * Set I/O limit (IOPS and bandwidth) for all block devices.
     * Uses io.max in cgroup v2.
     * Format: "MAJ:MIN riops=N wiops=N rbps=N wbps=N"
     */
    VoidResult<> set_io_limit(const std::string& app, uint64_t iops,
                               uint64_t bandwidth_bytes) {
        std::string cg_path = get_cgroup_path(app);

        // Get all block devices
        auto devices = get_block_devices();

        std::ostringstream ss;
        for (const auto& dev : devices) {
            ss << dev;
            if (iops > 0)
                ss << " riops=" << iops << " wiops=" << iops;
            if (bandwidth_bytes > 0)
                ss << " rbps=" << bandwidth_bytes << " wbps=" << bandwidth_bytes;
            ss << "\n";
        }

        return write_file(cg_path + "/io.max", ss.str());
    }

    /**
     * Read current resource usage for an app from cgroup controllers.
     */
    Result<CgroupUsage, std::string> get_usage(const std::string& app) {
        std::string cg_path = get_cgroup_path(app);
        CgroupUsage usage{};

        // CPU usage from cpu.stat
        auto cpu_stat = read_file(cg_path + "/cpu.stat");
        if (!cpu_stat.empty()) {
            // Parse "usage_usec NNN"
            auto pos = cpu_stat.find("usage_usec ");
            if (pos != std::string::npos) {
                uint64_t usec = std::stoull(
                    cpu_stat.substr(pos + 11));
                // Convert to approximate percent (rough — needs delta calc)
                // Store raw microseconds; caller converts via sampling
                usage.cpu_percent = static_cast<double>(usec) / 1000000.0;
            }
        }

        // Memory usage from memory.current
        auto mem = read_file(cg_path + "/memory.current");
        if (!mem.empty()) {
            try { usage.ram_bytes = std::stoull(mem); } catch (...) {}
        }

        // I/O usage from io.stat
        auto io_stat = read_file(cg_path + "/io.stat");
        if (!io_stat.empty()) {
            std::istringstream iss(io_stat);
            std::string line;
            while (std::getline(iss, line)) {
                // Parse "MAJ:MIN rbytes=N wbytes=N rios=N wios=N ..."
                uint64_t rb = 0, wb = 0, ri = 0, wi = 0;
                parse_io_stat_line(line, rb, wb, ri, wi);
                usage.disk_read_bytes += rb;
                usage.disk_write_bytes += wb;
                usage.disk_iops += ri + wi;
            }
        }

        return Result<CgroupUsage, std::string>::ok(usage);
    }

    /**
     * Remove a cgroup (after migrating all processes out).
     */
    VoidResult<> remove_cgroup(const std::string& app) {
        std::lock_guard<std::mutex> lock(mu_);
        std::string cg_path = get_cgroup_path(app);

        // Move all procs to parent cgroup first
        auto procs = read_file(cg_path + "/cgroup.procs");
        std::istringstream iss(procs);
        std::string pid_str;
        while (iss >> pid_str) {
            write_file(std::string(CGROUP_ROOT) + "/cgroup.procs", pid_str);
        }

        // Remove the directory
        std::error_code ec;
        std::filesystem::remove(cg_path, ec);
        known_cgroups_.erase(app);

        return VoidResult<>::ok();
    }

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::string> known_cgroups_;

    std::string get_cgroup_path(const std::string& app) const {
        return std::string(CGROUP_ROOT) + "/" + app;
    }

    static VoidResult<> write_file(const std::string& path,
                                     const std::string& content) {
        std::ofstream out(path);
        if (!out) {
            return VoidResult<>::error("cannot write " + path);
        }
        out << content;
        if (!out.good()) {
            return VoidResult<>::error("write error on " + path);
        }
        return VoidResult<>::ok();
    }

    static std::string read_file(const std::string& path) {
        std::ifstream in(path);
        if (!in) return "";
        std::string content((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
        // Trim trailing whitespace
        while (!content.empty() &&
               (content.back() == '\n' || content.back() == ' '))
            content.pop_back();
        return content;
    }

    static std::vector<std::string> get_block_devices() {
        std::vector<std::string> devices;
        namespace fs = std::filesystem;
        std::error_code ec;
        for (auto& entry : fs::directory_iterator("/sys/block", ec)) {
            std::string dev_name = entry.path().filename().string();
            // Read major:minor from dev file
            std::string dev_path = "/sys/block/" + dev_name + "/dev";
            std::string dev_id = read_file(dev_path);
            if (!dev_id.empty()) {
                devices.push_back(dev_id);
            }
        }
        // Fallback if /sys/block doesn't exist
        if (devices.empty()) {
            devices.push_back("8:0"); // sda default
        }
        return devices;
    }

    static void parse_io_stat_line(const std::string& line,
                                    uint64_t& rbytes, uint64_t& wbytes,
                                    uint64_t& rios, uint64_t& wios) {
        auto find_val = [&](const std::string& key) -> uint64_t {
            auto pos = line.find(key + "=");
            if (pos == std::string::npos) return 0;
            pos += key.size() + 1;
            try { return std::stoull(line.substr(pos)); } catch (...) { return 0; }
        };

        rbytes = find_val("rbytes");
        wbytes = find_val("wbytes");
        rios = find_val("rios");
        wios = find_val("wios");
    }
};

} // namespace straylight::quota
