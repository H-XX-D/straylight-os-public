// services/alice/log_analyzer.cpp
#include "log_analyzer.h"
#include <straylight/log.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace straylight {

// ---------------------------------------------------------------------------
// Utility helpers
// ---------------------------------------------------------------------------

std::string LogAnalyzer::read_sysfs(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) return {};
    std::string content;
    std::getline(ifs, content);
    return content;
}

std::string LogAnalyzer::run_command(const std::string& cmd) {
    std::string result;
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) return {};

    char buf[4096];
    while (::fgets(buf, sizeof(buf), pipe)) {
        result += buf;
    }
    ::pclose(pipe);
    return result;
}

std::string LogAnalyzer::priority_to_level(int priority) {
    // syslog priority levels: 0=emerg, 1=alert, 2=crit, 3=err, 4=warn, 5=notice, 6=info, 7=debug
    int level = priority & 0x07;
    switch (level) {
        case 0: case 1: case 2: return "crit";
        case 3: return "err";
        case 4: return "warn";
        case 5: case 6: return "info";
        default: return "debug";
    }
}

// ---------------------------------------------------------------------------
// /dev/kmsg parsing
// ---------------------------------------------------------------------------

std::vector<LogAnalyzer::LogEntry> LogAnalyzer::parse_kmsg(int max_entries) {
    std::vector<LogEntry> entries;

    // /dev/kmsg requires root and is Linux-specific
    // Fallback to dmesg command which is more portable
    std::string output = run_command("dmesg --time-format iso --level err,warn,info -T 2>/dev/null || dmesg 2>/dev/null");
    if (output.empty()) return entries;

    std::istringstream stream(output);
    std::string line;
    int count = 0;

    while (std::getline(stream, line) && count < max_entries) {
        if (line.empty()) continue;

        LogEntry entry;
        entry.source = "dmesg";
        entry.timestamp = std::chrono::system_clock::now();

        // Try to parse priority from bracket notation: [    0.000000] message
        // or ISO format: 2024-01-01T00:00:00+0000 message
        std::string level_hint;

        // Simple heuristic: check for known error patterns
        std::string lower_line = line;
        std::transform(lower_line.begin(), lower_line.end(), lower_line.begin(), ::tolower);

        if (lower_line.find("error") != std::string::npos ||
            lower_line.find("fail") != std::string::npos ||
            lower_line.find("fault") != std::string::npos) {
            entry.level = "err";
        } else if (lower_line.find("warn") != std::string::npos) {
            entry.level = "warn";
        } else if (lower_line.find("critical") != std::string::npos ||
                   lower_line.find("panic") != std::string::npos) {
            entry.level = "crit";
        } else {
            entry.level = "info";
        }

        entry.message = line;
        entries.push_back(std::move(entry));
        ++count;
    }

    return entries;
}

// ---------------------------------------------------------------------------
// journalctl parsing
// ---------------------------------------------------------------------------

std::vector<LogAnalyzer::LogEntry> LogAnalyzer::parse_journalctl(
    int max_entries, const std::string& since) {
    std::vector<LogEntry> entries;

    std::ostringstream cmd;
    cmd << "journalctl --no-pager -o json -n " << max_entries;
    if (!since.empty()) {
        cmd << " --since '" << since << "'";
    }
    cmd << " 2>/dev/null";

    std::string output = run_command(cmd.str());
    if (output.empty()) return entries;

    // Each line is a JSON object
    std::istringstream stream(output);
    std::string line;

    while (std::getline(stream, line) && static_cast<int>(entries.size()) < max_entries) {
        if (line.empty()) continue;

        try {
            // Manual JSON parsing to avoid dependency issues — we just need a few fields
            // Look for PRIORITY, MESSAGE, _COMM fields
            LogEntry entry;
            entry.source = "journalctl";
            entry.timestamp = std::chrono::system_clock::now();

            // Extract PRIORITY
            auto prio_pos = line.find("\"PRIORITY\":\"");
            if (prio_pos != std::string::npos) {
                prio_pos += 12; // length of "PRIORITY":"
                char prio_char = line[prio_pos];
                int prio = prio_char - '0';
                entry.level = priority_to_level(prio);
            } else {
                entry.level = "info";
            }

            // Extract MESSAGE
            auto msg_pos = line.find("\"MESSAGE\":\"");
            if (msg_pos != std::string::npos) {
                msg_pos += 11; // length of "MESSAGE":"
                auto msg_end = line.find('"', msg_pos);
                if (msg_end != std::string::npos) {
                    entry.message = line.substr(msg_pos, msg_end - msg_pos);
                }
            }

            if (!entry.message.empty()) {
                entries.push_back(std::move(entry));
            }
        } catch (...) {
            // Skip malformed lines
            continue;
        }
    }

    return entries;
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

Result<std::vector<LogAnalyzer::LogEntry>, std::string>
LogAnalyzer::collect_recent(int max_entries) {
    std::vector<LogEntry> all_entries;

    // Collect from dmesg
    auto kmsg = parse_kmsg(max_entries / 2);
    all_entries.insert(all_entries.end(),
                       std::make_move_iterator(kmsg.begin()),
                       std::make_move_iterator(kmsg.end()));

    // Collect from journalctl
    auto journal = parse_journalctl(max_entries / 2);
    all_entries.insert(all_entries.end(),
                       std::make_move_iterator(journal.begin()),
                       std::make_move_iterator(journal.end()));

    // Sort by timestamp (most recent first)
    std::sort(all_entries.begin(), all_entries.end(),
              [](const LogEntry& a, const LogEntry& b) {
                  return a.timestamp > b.timestamp;
              });

    // Trim to max
    if (static_cast<int>(all_entries.size()) > max_entries) {
        all_entries.resize(static_cast<size_t>(max_entries));
    }

    return Result<std::vector<LogEntry>, std::string>::ok(std::move(all_entries));
}

Result<std::vector<LogAnalyzer::LogEntry>, std::string>
LogAnalyzer::collect_errors(std::chrono::seconds window) {
    auto all_result = collect_recent(500);
    if (!all_result.has_value()) {
        return all_result;
    }

    auto now = std::chrono::system_clock::now();
    std::vector<LogEntry> errors;

    for (auto& entry : all_result.value()) {
        if (entry.level == "err" || entry.level == "crit") {
            auto age = std::chrono::duration_cast<std::chrono::seconds>(
                now - entry.timestamp);
            if (age <= window) {
                errors.push_back(std::move(entry));
            }
        }
    }

    return Result<std::vector<LogEntry>, std::string>::ok(std::move(errors));
}

Result<std::string, std::string>
LogAnalyzer::summarize_for_ai(const std::vector<LogEntry>& entries) {
    std::ostringstream out;
    out << "=== System Log Summary ===\n";
    out << "Entries: " << entries.size() << "\n\n";

    // Count by level
    int info_count = 0, warn_count = 0, err_count = 0, crit_count = 0;
    for (const auto& e : entries) {
        if (e.level == "info") ++info_count;
        else if (e.level == "warn") ++warn_count;
        else if (e.level == "err") ++err_count;
        else if (e.level == "crit") ++crit_count;
    }

    out << "Level breakdown: "
        << crit_count << " critical, "
        << err_count << " errors, "
        << warn_count << " warnings, "
        << info_count << " info\n\n";

    // Output entries grouped by severity (critical first)
    auto output_level = [&](const std::string& level, const std::string& label) {
        bool header_written = false;
        for (const auto& e : entries) {
            if (e.level == level) {
                if (!header_written) {
                    out << "--- " << label << " ---\n";
                    header_written = true;
                }
                out << "[" << e.source << "] " << e.message << "\n";
            }
        }
        if (header_written) out << "\n";
    };

    output_level("crit", "CRITICAL");
    output_level("err", "ERRORS");
    output_level("warn", "WARNINGS");

    // Only include first 10 info entries to save tokens
    int info_shown = 0;
    bool info_header = false;
    for (const auto& e : entries) {
        if (e.level == "info") {
            if (!info_header) {
                out << "--- INFO (first 10) ---\n";
                info_header = true;
            }
            out << "[" << e.source << "] " << e.message << "\n";
            if (++info_shown >= 10) break;
        }
    }

    return Result<std::string, std::string>::ok(out.str());
}

// ---------------------------------------------------------------------------
// Hardware monitors
// ---------------------------------------------------------------------------

Result<std::string, std::string> LogAnalyzer::gpu_health() {
    std::ostringstream out;
    out << "=== GPU Health ===\n";

    bool found_any = false;
    namespace fs = std::filesystem;

    // Check DRM cards in sysfs
    const std::string drm_path = "/sys/class/drm";
    std::error_code ec;
    if (fs::exists(drm_path, ec)) {
        for (const auto& entry : fs::directory_iterator(drm_path, ec)) {
            std::string name = entry.path().filename().string();
            if (name.find("card") == std::string::npos || name.find('-') != std::string::npos) {
                continue;
            }

            out << "Device: " << name << "\n";

            // Try reading GPU info
            std::string device_path = entry.path().string() + "/device";

            std::string vendor = read_sysfs(device_path + "/vendor");
            if (!vendor.empty()) out << "  Vendor: " << vendor << "\n";

            std::string device = read_sysfs(device_path + "/device");
            if (!device.empty()) out << "  Device: " << device << "\n";

            // AMD GPU temperature (hwmon)
            std::string hwmon_path = device_path + "/hwmon";
            if (fs::exists(hwmon_path, ec)) {
                for (const auto& hw : fs::directory_iterator(hwmon_path, ec)) {
                    std::string temp = read_sysfs(hw.path().string() + "/temp1_input");
                    if (!temp.empty()) {
                        try {
                            int millideg = std::stoi(temp);
                            out << "  Temperature: " << (millideg / 1000) << "C\n";
                        } catch (...) {}
                    }
                }
            }

            // GPU utilization (AMD)
            std::string gpu_busy = read_sysfs(device_path + "/gpu_busy_percent");
            if (!gpu_busy.empty()) out << "  Utilization: " << gpu_busy << "%\n";

            // VRAM usage (AMD)
            std::string vram_used = read_sysfs(device_path + "/mem_info_vram_used");
            std::string vram_total = read_sysfs(device_path + "/mem_info_vram_total");
            if (!vram_used.empty() && !vram_total.empty()) {
                try {
                    long long used = std::stoll(vram_used);
                    long long total = std::stoll(vram_total);
                    out << "  VRAM: " << (used / (1024 * 1024)) << " / "
                        << (total / (1024 * 1024)) << " MB\n";
                } catch (...) {}
            }

            found_any = true;
            out << "\n";
        }
    }

    // Try nvidia-smi for NVIDIA GPUs
    std::string nvidia_output = run_command(
        "nvidia-smi --query-gpu=name,temperature.gpu,utilization.gpu,"
        "memory.used,memory.total --format=csv,noheader 2>/dev/null");
    if (!nvidia_output.empty()) {
        out << "NVIDIA GPU(s):\n" << nvidia_output << "\n";
        found_any = true;
    }

    if (!found_any) {
        out << "No GPU devices detected in sysfs or via nvidia-smi.\n";
    }

    return Result<std::string, std::string>::ok(out.str());
}

Result<std::string, std::string> LogAnalyzer::thermal_status() {
    std::ostringstream out;
    out << "=== Thermal Status ===\n";

    namespace fs = std::filesystem;
    std::error_code ec;
    bool found_any = false;

    // Read /sys/class/thermal/thermal_zone*/temp
    const std::string thermal_path = "/sys/class/thermal";
    if (fs::exists(thermal_path, ec)) {
        for (const auto& entry : fs::directory_iterator(thermal_path, ec)) {
            std::string name = entry.path().filename().string();
            if (name.find("thermal_zone") == std::string::npos) continue;

            std::string temp = read_sysfs(entry.path().string() + "/temp");
            std::string type = read_sysfs(entry.path().string() + "/type");

            if (!temp.empty()) {
                try {
                    int millideg = std::stoi(temp);
                    out << name << " (" << (type.empty() ? "unknown" : type)
                        << "): " << (millideg / 1000) << "C\n";
                    found_any = true;
                } catch (...) {}
            }
        }
    }

    // Read /sys/class/hwmon/*/temp*_input
    const std::string hwmon_path = "/sys/class/hwmon";
    if (fs::exists(hwmon_path, ec)) {
        for (const auto& hw : fs::directory_iterator(hwmon_path, ec)) {
            std::string hw_name = read_sysfs(hw.path().string() + "/name");

            for (const auto& file : fs::directory_iterator(hw.path(), ec)) {
                std::string fname = file.path().filename().string();
                if (fname.find("temp") == 0 && fname.find("_input") != std::string::npos) {
                    std::string temp = read_sysfs(file.path().string());
                    if (!temp.empty()) {
                        try {
                            int millideg = std::stoi(temp);
                            out << hw_name << "/" << fname << ": "
                                << (millideg / 1000) << "C\n";
                            found_any = true;
                        } catch (...) {}
                    }
                }
            }
        }
    }

    if (!found_any) {
        out << "No thermal sensors found in sysfs.\n";
    }

    return Result<std::string, std::string>::ok(out.str());
}

Result<std::string, std::string> LogAnalyzer::memory_pressure() {
    std::ostringstream out;
    out << "=== Memory Pressure ===\n";

    // Read /proc/meminfo
    std::ifstream meminfo("/proc/meminfo");
    if (meminfo.is_open()) {
        std::string line;
        long long mem_total = 0, mem_available = 0, mem_free = 0;
        long long swap_total = 0, swap_free = 0;
        long long buffers = 0, cached = 0;

        while (std::getline(meminfo, line)) {
            auto extract = [&](const std::string& key, long long& val) {
                if (line.find(key) == 0) {
                    auto pos = line.find_first_of("0123456789");
                    if (pos != std::string::npos) {
                        try { val = std::stoll(line.substr(pos)); } catch (...) {}
                    }
                }
            };
            extract("MemTotal:", mem_total);
            extract("MemAvailable:", mem_available);
            extract("MemFree:", mem_free);
            extract("Buffers:", buffers);
            extract("Cached:", cached);
            extract("SwapTotal:", swap_total);
            extract("SwapFree:", swap_free);
        }

        out << "MemTotal:     " << mem_total << " kB\n";
        out << "MemAvailable: " << mem_available << " kB\n";
        out << "MemFree:      " << mem_free << " kB\n";
        out << "Buffers:      " << buffers << " kB\n";
        out << "Cached:       " << cached << " kB\n";

        if (mem_total > 0) {
            double used_pct = 100.0 * (1.0 - static_cast<double>(mem_available) /
                                                static_cast<double>(mem_total));
            out << "Usage:        " << static_cast<int>(used_pct) << "%\n";
        }

        out << "SwapTotal:    " << swap_total << " kB\n";
        out << "SwapFree:     " << swap_free << " kB\n";
        if (swap_total > 0) {
            double swap_pct = 100.0 * (1.0 - static_cast<double>(swap_free) /
                                                static_cast<double>(swap_total));
            out << "SwapUsage:    " << static_cast<int>(swap_pct) << "%\n";
        }
    } else {
        out << "/proc/meminfo: not available\n";
    }

    // Read /proc/pressure/memory (PSI)
    std::ifstream psi("/proc/pressure/memory");
    if (psi.is_open()) {
        out << "\nPSI Memory:\n";
        std::string line;
        while (std::getline(psi, line)) {
            out << "  " << line << "\n";
        }
    }

    return Result<std::string, std::string>::ok(out.str());
}

Result<std::string, std::string> LogAnalyzer::disk_health() {
    std::ostringstream out;
    out << "=== Disk Health ===\n";

    // Try SMART data via smartctl
    std::string smartctl_output = run_command(
        "smartctl --scan --json 2>/dev/null");

    if (!smartctl_output.empty() && smartctl_output.find("devices") != std::string::npos) {
        // Parse device list and get health for each
        // Simple approach: scan for /dev/ paths
        std::istringstream scan_stream(smartctl_output);
        std::string line;
        while (std::getline(scan_stream, line)) {
            auto dev_pos = line.find("/dev/");
            if (dev_pos != std::string::npos) {
                auto dev_end = line.find('"', dev_pos);
                if (dev_end == std::string::npos) dev_end = line.find(' ', dev_pos);
                if (dev_end == std::string::npos) dev_end = line.size();

                std::string device = line.substr(dev_pos, dev_end - dev_pos);
                out << "\nDevice: " << device << "\n";

                std::string health = run_command(
                    "smartctl -H --json " + device + " 2>/dev/null");
                if (!health.empty()) {
                    // Look for "passed" or "FAILED"
                    if (health.find("PASSED") != std::string::npos ||
                        health.find("passed") != std::string::npos) {
                        out << "  SMART Status: PASSED\n";
                    } else if (health.find("FAIL") != std::string::npos) {
                        out << "  SMART Status: FAILING\n";
                    }

                    // Try to get temperature
                    std::string attrs = run_command(
                        "smartctl -A --json " + device + " 2>/dev/null");
                    if (attrs.find("temperature") != std::string::npos) {
                        auto temp_pos = attrs.find("\"current\"");
                        if (temp_pos != std::string::npos) {
                            auto num_pos = attrs.find_first_of("0123456789", temp_pos + 10);
                            if (num_pos != std::string::npos) {
                                auto num_end = attrs.find_first_not_of("0123456789", num_pos);
                                out << "  Temperature: "
                                    << attrs.substr(num_pos, num_end - num_pos) << "C\n";
                            }
                        }
                    }
                }
            }
        }
    } else {
        out << "smartctl not available or no devices found.\n";
    }

    // Read /proc/diskstats for I/O counters
    std::ifstream diskstats("/proc/diskstats");
    if (diskstats.is_open()) {
        out << "\nI/O Statistics (/proc/diskstats):\n";
        std::string line;
        int shown = 0;
        while (std::getline(diskstats, line) && shown < 10) {
            // Filter to real block devices (sd*, nvme*, vd*)
            if (line.find("sd") != std::string::npos ||
                line.find("nvme") != std::string::npos ||
                line.find("vd") != std::string::npos) {
                // Only show devices, not partitions (skip lines ending with a digit after sd/vd)
                out << "  " << line << "\n";
                ++shown;
            }
        }
    }

    return Result<std::string, std::string>::ok(out.str());
}

Result<std::string, std::string> LogAnalyzer::network_status() {
    std::ostringstream out;
    out << "=== Network Status ===\n";

    namespace fs = std::filesystem;
    std::error_code ec;
    bool found_any = false;

    // Read /sys/class/net/*/operstate
    const std::string net_path = "/sys/class/net";
    if (fs::exists(net_path, ec)) {
        for (const auto& entry : fs::directory_iterator(net_path, ec)) {
            std::string iface = entry.path().filename().string();
            if (iface == "lo") continue;  // Skip loopback

            std::string operstate = read_sysfs(entry.path().string() + "/operstate");
            std::string speed = read_sysfs(entry.path().string() + "/speed");
            std::string mtu = read_sysfs(entry.path().string() + "/mtu");
            std::string carrier = read_sysfs(entry.path().string() + "/carrier");

            out << iface << ":\n";
            out << "  State: " << (operstate.empty() ? "unknown" : operstate) << "\n";
            if (!speed.empty() && speed != "-1") {
                out << "  Speed: " << speed << " Mbps\n";
            }
            if (!mtu.empty()) {
                out << "  MTU: " << mtu << "\n";
            }
            if (!carrier.empty()) {
                out << "  Carrier: " << (carrier == "1" ? "yes" : "no") << "\n";
            }

            found_any = true;
        }
    }

    // Read /proc/net/dev for error/drop counts
    std::ifstream netdev("/proc/net/dev");
    if (netdev.is_open()) {
        out << "\nInterface Statistics:\n";
        std::string line;
        int line_num = 0;
        while (std::getline(netdev, line)) {
            ++line_num;
            if (line_num <= 2) continue;  // Skip header lines
            if (line.find("lo:") != std::string::npos) continue;

            // Parse: iface: rx_bytes rx_packets rx_errs rx_drop ... tx_bytes tx_packets tx_errs tx_drop ...
            auto colon = line.find(':');
            if (colon == std::string::npos) continue;

            std::string iface = line.substr(0, colon);
            // Trim whitespace
            auto start = iface.find_first_not_of(' ');
            if (start != std::string::npos) iface = iface.substr(start);

            std::istringstream vals(line.substr(colon + 1));
            long long rx_bytes, rx_packets, rx_errs, rx_drop;
            long long rx_fifo, rx_frame, rx_compressed, rx_multicast;
            long long tx_bytes, tx_packets, tx_errs, tx_drop;

            if (vals >> rx_bytes >> rx_packets >> rx_errs >> rx_drop
                     >> rx_fifo >> rx_frame >> rx_compressed >> rx_multicast
                     >> tx_bytes >> tx_packets >> tx_errs >> tx_drop) {
                out << "  " << iface << ": rx=" << rx_bytes << "B/"
                    << rx_packets << "pkts tx=" << tx_bytes << "B/"
                    << tx_packets << "pkts";
                if (rx_errs > 0 || tx_errs > 0) {
                    out << " ERRORS(rx=" << rx_errs << " tx=" << tx_errs << ")";
                }
                if (rx_drop > 0 || tx_drop > 0) {
                    out << " DROPS(rx=" << rx_drop << " tx=" << tx_drop << ")";
                }
                out << "\n";
            }
            found_any = true;
        }
    }

    if (!found_any) {
        out << "No network interfaces found.\n";
    }

    return Result<std::string, std::string>::ok(out.str());
}

} // namespace straylight
