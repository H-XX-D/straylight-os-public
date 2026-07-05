// services/remote/system_info.cpp
#include "system_info.h"
#include <straylight/log.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/sysinfo.h>
#endif

namespace straylight {

SystemInfo::SystemInfo() = default;
SystemInfo::~SystemInfo() = default;

nlohmann::json SystemInfo::gather() {
    nlohmann::json info;
    info["cpu"] = gather_cpu();
    info["ram"] = gather_ram();
    info["gpus"] = gather_gpus();
    info["disks"] = gather_disks();
    info["network"] = gather_network();
    info["os"] = gather_os();
    info["services"] = gather_services();
    return info;
}

nlohmann::json SystemInfo::gather_cpu() {
    nlohmann::json cpu;

    // Read /proc/cpuinfo for model and core count
    std::string cpuinfo = read_file_contents("/proc/cpuinfo");
    if (!cpuinfo.empty()) {
        std::istringstream iss(cpuinfo);
        std::string line;
        int core_count = 0;
        std::string model_name;
        double mhz = 0.0;

        while (std::getline(iss, line)) {
            if (line.find("model name") == 0) {
                auto pos = line.find(':');
                if (pos != std::string::npos) {
                    model_name = trim(line.substr(pos + 1));
                }
            }
            if (line.find("processor") == 0) {
                core_count++;
            }
            if (line.find("cpu MHz") == 0) {
                auto pos = line.find(':');
                if (pos != std::string::npos) {
                    try {
                        mhz = std::stod(trim(line.substr(pos + 1)));
                    } catch (...) {}
                }
            }
        }

        cpu["model"] = model_name;
        cpu["cores"] = core_count;
        cpu["frequency_mhz"] = mhz;
    } else {
        // Fallback for non-Linux
        cpu["model"] = "unknown";
        cpu["cores"] = static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));
        cpu["frequency_mhz"] = 0;
    }

    // Load averages
    std::string loadavg = read_file_contents("/proc/loadavg");
    if (!loadavg.empty()) {
        std::istringstream iss(loadavg);
        double l1 = 0, l5 = 0, l15 = 0;
        iss >> l1 >> l5 >> l15;
        cpu["load_avg_1m"] = l1;
        cpu["load_avg_5m"] = l5;
        cpu["load_avg_15m"] = l15;
    } else {
        // macOS fallback
        std::string uptime_out = run_command("sysctl -n vm.loadavg 2>/dev/null");
        cpu["load_avg_1m"] = 0.0;
        cpu["load_avg_5m"] = 0.0;
        cpu["load_avg_15m"] = 0.0;
    }

    // Per-core usage from /proc/stat
    std::string stat = read_file_contents("/proc/stat");
    if (!stat.empty()) {
        nlohmann::json per_core = nlohmann::json::array();
        std::istringstream iss(stat);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.find("cpu") == 0 && line.size() > 3 && std::isdigit(line[3])) {
                std::istringstream ls(line);
                std::string core_name;
                uint64_t user, nice, sys, idle, iowait, irq, softirq, steal;
                ls >> core_name >> user >> nice >> sys >> idle >> iowait >> irq >> softirq >> steal;

                uint64_t total = user + nice + sys + idle + iowait + irq + softirq + steal;
                uint64_t busy = total - idle - iowait;
                double usage_pct = (total > 0) ? (100.0 * static_cast<double>(busy) / static_cast<double>(total)) : 0.0;

                per_core.push_back({
                    {"core", core_name},
                    {"usage_pct", usage_pct},
                    {"user", user},
                    {"system", sys},
                    {"idle", idle}
                });
            }
        }
        cpu["per_core"] = per_core;
    }

    return cpu;
}

nlohmann::json SystemInfo::gather_ram() {
    nlohmann::json ram;

    std::string meminfo = read_file_contents("/proc/meminfo");
    if (!meminfo.empty()) {
        uint64_t total_kb = 0, free_kb = 0, available_kb = 0;
        uint64_t swap_total_kb = 0, swap_free_kb = 0;
        uint64_t buffers_kb = 0, cached_kb = 0;

        std::istringstream iss(meminfo);
        std::string line;
        while (std::getline(iss, line)) {
            std::istringstream ls(line);
            std::string key;
            uint64_t val;
            ls >> key >> val;

            if (key == "MemTotal:") total_kb = val;
            else if (key == "MemFree:") free_kb = val;
            else if (key == "MemAvailable:") available_kb = val;
            else if (key == "Buffers:") buffers_kb = val;
            else if (key == "Cached:") cached_kb = val;
            else if (key == "SwapTotal:") swap_total_kb = val;
            else if (key == "SwapFree:") swap_free_kb = val;
        }

        uint64_t used_kb = total_kb - available_kb;

        ram["total_mb"] = total_kb / 1024;
        ram["used_mb"] = used_kb / 1024;
        ram["available_mb"] = available_kb / 1024;
        ram["free_mb"] = free_kb / 1024;
        ram["buffers_mb"] = buffers_kb / 1024;
        ram["cached_mb"] = cached_kb / 1024;
        ram["swap_total_mb"] = swap_total_kb / 1024;
        ram["swap_used_mb"] = (swap_total_kb - swap_free_kb) / 1024;
    } else {
        // Fallback
#ifdef __linux__
        struct sysinfo si{};
        if (sysinfo(&si) == 0) {
            ram["total_mb"] = (si.totalram * si.mem_unit) / (1024 * 1024);
            ram["used_mb"] = ((si.totalram - si.freeram) * si.mem_unit) / (1024 * 1024);
            ram["available_mb"] = (si.freeram * si.mem_unit) / (1024 * 1024);
            ram["swap_total_mb"] = (si.totalswap * si.mem_unit) / (1024 * 1024);
            ram["swap_used_mb"] = ((si.totalswap - si.freeswap) * si.mem_unit) / (1024 * 1024);
        }
#else
        // macOS fallback
        long pages = sysconf(_SC_PHYS_PAGES);
        long page_size = sysconf(_SC_PAGE_SIZE);
        ram["total_mb"] = (pages * page_size) / (1024 * 1024);
        ram["used_mb"] = 0;
        ram["available_mb"] = 0;
        ram["swap_total_mb"] = 0;
        ram["swap_used_mb"] = 0;
#endif
    }

    return ram;
}

nlohmann::json SystemInfo::gather_gpus() {
    nlohmann::json gpus = nlohmann::json::array();

    // Try NVIDIA GPUs via nvidia-smi
    std::string nvidia_out = run_command(
        "nvidia-smi --query-gpu=name,memory.total,memory.used,temperature.gpu,utilization.gpu "
        "--format=csv,noheader,nounits 2>/dev/null");

    if (!nvidia_out.empty()) {
        std::istringstream iss(nvidia_out);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.empty()) continue;
            std::istringstream ls(line);
            std::string name, mem_total, mem_used, temp, util;
            std::getline(ls, name, ',');
            std::getline(ls, mem_total, ',');
            std::getline(ls, mem_used, ',');
            std::getline(ls, temp, ',');
            std::getline(ls, util, ',');

            gpus.push_back({
                {"name", trim(name)},
                {"vendor", "nvidia"},
                {"memory_total_mb", trim(mem_total)},
                {"memory_used_mb", trim(mem_used)},
                {"temperature_c", trim(temp)},
                {"utilization_pct", trim(util)}
            });
        }
    }

    // Try AMD GPUs via rocm-smi
    std::string amd_out = run_command(
        "rocm-smi --showproductname --showmeminfo vram --showtemp --showuse "
        "--csv 2>/dev/null");

    if (!amd_out.empty() && amd_out.find("ERROR") == std::string::npos) {
        // Parse rocm-smi CSV output
        std::istringstream iss(amd_out);
        std::string line;
        bool header = true;
        while (std::getline(iss, line)) {
            if (header) { header = false; continue; }
            if (line.empty()) continue;

            gpus.push_back({
                {"name", line},
                {"vendor", "amd"},
                {"raw_output", line}
            });
        }
    }

    // Enumerate from /sys/class/drm/ as fallback
    if (gpus.empty()) {
        try {
            for (const auto& entry : std::filesystem::directory_iterator("/sys/class/drm/")) {
                std::string name = entry.path().filename().string();
                if (name.find("card") == 0 && name.find('-') == std::string::npos) {
                    std::string device_path = entry.path().string() + "/device";
                    std::string vendor = read_file_contents(device_path + "/vendor");
                    std::string device = read_file_contents(device_path + "/device");

                    gpus.push_back({
                        {"name", name},
                        {"vendor_id", trim(vendor)},
                        {"device_id", trim(device)},
                        {"sysfs", entry.path().string()}
                    });
                }
            }
        } catch (...) {
            // /sys/class/drm/ may not exist (e.g., macOS)
        }
    }

    return gpus;
}

nlohmann::json SystemInfo::gather_disks() {
    nlohmann::json disks = nlohmann::json::array();

    // Read mount points from /proc/mounts or /etc/mtab
    std::string mounts = read_file_contents("/proc/mounts");
    if (mounts.empty()) {
        mounts = read_file_contents("/etc/mtab");
    }

    if (!mounts.empty()) {
        std::istringstream iss(mounts);
        std::string line;
        while (std::getline(iss, line)) {
            std::istringstream ls(line);
            std::string device, mount_point, fs_type, options;
            ls >> device >> mount_point >> fs_type >> options;

            // Skip virtual filesystems
            if (fs_type == "proc" || fs_type == "sysfs" || fs_type == "devtmpfs" ||
                fs_type == "tmpfs" || fs_type == "devpts" || fs_type == "cgroup" ||
                fs_type == "cgroup2" || fs_type == "securityfs" || fs_type == "pstore" ||
                fs_type == "debugfs" || fs_type == "hugetlbfs" || fs_type == "mqueue" ||
                fs_type == "fusectl" || fs_type == "configfs" || fs_type == "binfmt_misc" ||
                fs_type == "autofs" || fs_type == "tracefs" || fs_type == "overlay") {
                continue;
            }

            struct statvfs vfs{};
            if (statvfs(mount_point.c_str(), &vfs) == 0) {
                uint64_t total = vfs.f_blocks * vfs.f_frsize;
                uint64_t free = vfs.f_bfree * vfs.f_frsize;
                uint64_t avail = vfs.f_bavail * vfs.f_frsize;
                uint64_t used = total - free;

                double usage_pct = (total > 0) ?
                    (100.0 * static_cast<double>(used) / static_cast<double>(total)) : 0.0;

                disks.push_back({
                    {"device", device},
                    {"mount", mount_point},
                    {"fs_type", fs_type},
                    {"total_gb", static_cast<double>(total) / (1024.0 * 1024 * 1024)},
                    {"used_gb", static_cast<double>(used) / (1024.0 * 1024 * 1024)},
                    {"available_gb", static_cast<double>(avail) / (1024.0 * 1024 * 1024)},
                    {"usage_pct", usage_pct}
                });
            }
        }
    } else {
        // Fallback: use df command
        std::string df_out = run_command("df -h 2>/dev/null");
        if (!df_out.empty()) {
            disks.push_back({{"raw", df_out}});
        }
    }

    // SMART status for physical drives
    std::string smart_out = run_command(
        "smartctl --scan 2>/dev/null | while read dev rest; do "
        "echo \"$dev: $(smartctl -H $dev 2>/dev/null | grep 'SMART overall' || echo 'N/A')\"; "
        "done");
    if (!smart_out.empty()) {
        nlohmann::json smart = nlohmann::json::array();
        std::istringstream iss(smart_out);
        std::string line;
        while (std::getline(iss, line)) {
            if (!line.empty()) {
                smart.push_back(line);
            }
        }
        if (!smart.empty()) {
            // Attach SMART info to the disk array's root
            for (auto& disk : disks) {
                disk["smart"] = smart;
                break;  // Only attach to first entry for now
            }
        }
    }

    return disks;
}

nlohmann::json SystemInfo::gather_network() {
    nlohmann::json interfaces = nlohmann::json::array();

    // Read from /sys/class/net/
    try {
        for (const auto& entry : std::filesystem::directory_iterator("/sys/class/net/")) {
            std::string iface = entry.path().filename().string();

            // Skip loopback unless it's the only interface
            nlohmann::json info;
            info["name"] = iface;

            // Read operstate
            std::string operstate = trim(read_file_contents(
                entry.path().string() + "/operstate"));
            info["state"] = operstate;

            // Read speed (link speed in Mbps)
            std::string speed = trim(read_file_contents(
                entry.path().string() + "/speed"));
            if (!speed.empty() && speed != "-1") {
                try {
                    info["speed_mbps"] = std::stoi(speed);
                } catch (...) {
                    info["speed_mbps"] = 0;
                }
            } else {
                info["speed_mbps"] = 0;
            }

            // Read MAC address
            std::string address = trim(read_file_contents(
                entry.path().string() + "/address"));
            info["mac"] = address;

            // Read TX/RX bytes
            std::string rx_bytes = trim(read_file_contents(
                entry.path().string() + "/statistics/rx_bytes"));
            std::string tx_bytes = trim(read_file_contents(
                entry.path().string() + "/statistics/tx_bytes"));

            try {
                info["rx_bytes"] = rx_bytes.empty() ? 0ULL : std::stoull(rx_bytes);
                info["tx_bytes"] = tx_bytes.empty() ? 0ULL : std::stoull(tx_bytes);
            } catch (...) {
                info["rx_bytes"] = 0;
                info["tx_bytes"] = 0;
            }

            // Get IP addresses via ip command
            std::string ip_out = run_command(
                "ip -j addr show " + iface + " 2>/dev/null");
            if (!ip_out.empty()) {
                try {
                    auto ip_json = nlohmann::json::parse(ip_out);
                    nlohmann::json addrs = nlohmann::json::array();
                    for (const auto& entry_json : ip_json) {
                        if (entry_json.contains("addr_info")) {
                            for (const auto& addr : entry_json["addr_info"]) {
                                addrs.push_back({
                                    {"address", addr.value("local", "")},
                                    {"prefix", addr.value("prefixlen", 0)},
                                    {"family", addr.value("family", "")}
                                });
                            }
                        }
                    }
                    info["addresses"] = addrs;
                } catch (...) {
                    // ip -j not available, try plain
                    info["addresses"] = nlohmann::json::array();
                }
            }

            interfaces.push_back(info);
        }
    } catch (...) {
        // /sys/class/net/ doesn't exist (e.g., macOS)
        // Fall back to ifconfig
        std::string ifconfig = run_command("ifconfig 2>/dev/null");
        if (!ifconfig.empty()) {
            interfaces.push_back({{"raw", ifconfig}});
        }
    }

    return interfaces;
}

nlohmann::json SystemInfo::gather_os() {
    nlohmann::json os;

    // Hostname
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        os["hostname"] = std::string(hostname);
    } else {
        os["hostname"] = "unknown";
    }

    // Kernel version via uname
    struct utsname uts{};
    if (uname(&uts) == 0) {
        os["kernel"] = std::string(uts.release);
        os["arch"] = std::string(uts.machine);
        os["sysname"] = std::string(uts.sysname);
    }

    // Uptime
    std::string uptime_str = read_file_contents("/proc/uptime");
    if (!uptime_str.empty()) {
        double uptime_secs = 0;
        std::istringstream iss(uptime_str);
        iss >> uptime_secs;
        os["uptime_seconds"] = static_cast<int64_t>(uptime_secs);
    } else {
        // Fallback: use sysctl on macOS
        std::string boot_time = run_command("sysctl -n kern.boottime 2>/dev/null");
        os["uptime_seconds"] = 0;
    }

    // StrayLight version
    std::string sl_version = read_file_contents("/etc/straylight/version");
    if (!sl_version.empty()) {
        os["straylight_version"] = trim(sl_version);
    } else {
        os["straylight_version"] = "unknown";
    }

    // OS release info
    std::string os_release = read_file_contents("/etc/os-release");
    if (!os_release.empty()) {
        std::istringstream iss(os_release);
        std::string line;
        while (std::getline(iss, line)) {
            auto eq = line.find('=');
            if (eq != std::string::npos) {
                std::string key = line.substr(0, eq);
                std::string val = line.substr(eq + 1);
                // Remove quotes
                if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
                    val = val.substr(1, val.size() - 2);
                }
                if (key == "PRETTY_NAME") os["distro"] = val;
                if (key == "VERSION_ID") os["distro_version"] = val;
            }
        }
    }

    return os;
}

nlohmann::json SystemInfo::gather_services() {
    nlohmann::json services = nlohmann::json::array();

    // List StrayLight-specific systemd units
    std::string out = run_command(
        "systemctl list-units 'straylight-*' --no-pager --plain --no-legend 2>/dev/null");

    if (!out.empty()) {
        std::istringstream iss(out);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.empty()) continue;
            std::istringstream ls(line);
            std::string unit, load, active, sub, description;
            ls >> unit >> load >> active >> sub;
            std::getline(ls, description);

            services.push_back({
                {"unit", unit},
                {"load", load},
                {"active", active},
                {"sub", sub},
                {"description", trim(description)}
            });
        }
    }

    return services;
}

std::string SystemInfo::read_file_contents(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return "";
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

std::string SystemInfo::run_command(const std::string& cmd) {
    std::array<char, 4096> buffer;
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        result += buffer.data();
    }
    pclose(pipe);
    return result;
}

std::string SystemInfo::trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\n\r");
    return s.substr(start, end - start + 1);
}

} // namespace straylight
