// tools/profile/profiler.cpp
#include "profiler.h"

#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace straylight {

namespace fs = std::filesystem;

std::string Profiler::run_cmd(const std::string& cmd) const {
    std::array<char, 4096> buffer;
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) return "";
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get())) {
        result += buffer.data();
    }
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
        result.pop_back();
    }
    return result;
}

std::vector<ProfileMetric> Profiler::collect_cpu() {
    std::vector<ProfileMetric> metrics;

    // CPU model
    std::string model = run_cmd("grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2");
    if (model.empty()) model = run_cmd("sysctl -n machdep.cpu.brand_string 2>/dev/null");
    while (!model.empty() && model.front() == ' ') model.erase(model.begin());
    metrics.push_back({"CPU", "Model", model, ""});

    // CPU cores
    std::string cores = run_cmd("nproc 2>/dev/null");
    if (cores.empty()) cores = run_cmd("sysctl -n hw.ncpu 2>/dev/null");
    metrics.push_back({"CPU", "Cores", cores, ""});

    // CPU frequency
    std::string freq = run_cmd(
        "grep -m1 'cpu MHz' /proc/cpuinfo 2>/dev/null | cut -d: -f2");
    if (!freq.empty()) {
        while (!freq.empty() && freq.front() == ' ') freq.erase(freq.begin());
        metrics.push_back({"CPU", "Frequency", freq, "MHz"});
    }

    // CPU architecture
    std::string arch = run_cmd("uname -m 2>/dev/null");
    metrics.push_back({"CPU", "Architecture", arch, ""});

    // Load average
    std::string load = run_cmd("cat /proc/loadavg 2>/dev/null | cut -d' ' -f1-3");
    if (load.empty()) load = run_cmd("sysctl -n vm.loadavg 2>/dev/null");
    metrics.push_back({"CPU", "Load Average", load, ""});

    return metrics;
}

std::vector<ProfileMetric> Profiler::collect_memory() {
    std::vector<ProfileMetric> metrics;

    std::string total = run_cmd(
        "grep MemTotal /proc/meminfo 2>/dev/null | awk '{printf \"%.1f\", $2/1048576}'");
    if (total.empty()) {
        total = run_cmd("sysctl -n hw.memsize 2>/dev/null");
        if (!total.empty()) {
            double bytes = std::stod(total);
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(1) << bytes / (1024.0 * 1024.0 * 1024.0);
            total = oss.str();
        }
    }
    metrics.push_back({"Memory", "Total", total, "GB"});

    std::string available = run_cmd(
        "grep MemAvailable /proc/meminfo 2>/dev/null | awk '{printf \"%.1f\", $2/1048576}'");
    if (!available.empty()) {
        metrics.push_back({"Memory", "Available", available, "GB"});
    }

    std::string swap = run_cmd(
        "grep SwapTotal /proc/meminfo 2>/dev/null | awk '{printf \"%.1f\", $2/1048576}'");
    if (!swap.empty()) {
        metrics.push_back({"Memory", "Swap Total", swap, "GB"});
    }

    return metrics;
}

std::vector<ProfileMetric> Profiler::collect_gpu() {
    std::vector<ProfileMetric> metrics;

    // Try nvidia-smi first
    std::string gpu = run_cmd(
        "nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null | head -1");
    if (!gpu.empty()) {
        metrics.push_back({"GPU", "Name", gpu, ""});
        std::string mem = run_cmd(
            "nvidia-smi --query-gpu=memory.total --format=csv,noheader 2>/dev/null | head -1");
        if (!mem.empty()) metrics.push_back({"GPU", "Memory", mem, ""});
        std::string driver = run_cmd(
            "nvidia-smi --query-gpu=driver_version --format=csv,noheader 2>/dev/null | head -1");
        if (!driver.empty()) metrics.push_back({"GPU", "Driver", driver, ""});
    } else {
        // Try lspci
        std::string vga = run_cmd("lspci 2>/dev/null | grep -i vga | cut -d: -f3");
        if (!vga.empty()) {
            while (!vga.empty() && vga.front() == ' ') vga.erase(vga.begin());
            metrics.push_back({"GPU", "Name", vga, ""});
        }
    }

    return metrics;
}

std::vector<ProfileMetric> Profiler::collect_disk() {
    std::vector<ProfileMetric> metrics;

    std::string df_out = run_cmd("df -h / 2>/dev/null | tail -1");
    if (!df_out.empty()) {
        std::istringstream iss(df_out);
        std::string dev, size, used, avail, pct, mount;
        iss >> dev >> size >> used >> avail >> pct >> mount;
        metrics.push_back({"Disk", "Root Device", dev, ""});
        metrics.push_back({"Disk", "Total Size", size, ""});
        metrics.push_back({"Disk", "Used", used, ""});
        metrics.push_back({"Disk", "Available", avail, ""});
        metrics.push_back({"Disk", "Usage", pct, ""});
    }

    // Check for NVMe devices
    std::string nvme = run_cmd("ls /dev/nvme* 2>/dev/null | wc -l");
    if (!nvme.empty() && nvme != "0") {
        metrics.push_back({"Disk", "NVMe Devices", nvme, ""});
    }

    return metrics;
}

std::vector<ProfileMetric> Profiler::collect_network() {
    std::vector<ProfileMetric> metrics;

    std::string hostname = run_cmd("hostname 2>/dev/null");
    metrics.push_back({"Network", "Hostname", hostname, ""});

    std::string iface = run_cmd(
        "ip route get 1.1.1.1 2>/dev/null | head -1 | awk '{print $5}'");
    if (!iface.empty()) {
        metrics.push_back({"Network", "Default Interface", iface, ""});
    }

    std::string ip = run_cmd(
        "ip route get 1.1.1.1 2>/dev/null | head -1 | awk '{print $7}'");
    if (ip.empty()) {
        ip = run_cmd("hostname -I 2>/dev/null | awk '{print $1}'");
    }
    if (!ip.empty()) {
        metrics.push_back({"Network", "IP Address", ip, ""});
    }

    std::string dns = run_cmd("cat /etc/resolv.conf 2>/dev/null | grep nameserver | head -1 | awk '{print $2}'");
    if (!dns.empty()) {
        metrics.push_back({"Network", "Primary DNS", dns, ""});
    }

    return metrics;
}

std::vector<ProfileMetric> Profiler::collect_kernel() {
    std::vector<ProfileMetric> metrics;
    std::string kernel = run_cmd("uname -r 2>/dev/null");
    metrics.push_back({"Kernel", "Version", kernel, ""});

    std::string os = run_cmd("cat /etc/os-release 2>/dev/null | grep PRETTY_NAME | cut -d= -f2 | tr -d '\"'");
    if (os.empty()) os = run_cmd("uname -s 2>/dev/null");
    metrics.push_back({"Kernel", "OS", os, ""});

    return metrics;
}

std::vector<ProfileMetric> Profiler::collect_boot() {
    std::vector<ProfileMetric> metrics;
    std::string uptime = run_cmd("uptime -p 2>/dev/null");
    if (uptime.empty()) uptime = run_cmd("uptime 2>/dev/null | awk -F'up ' '{print $2}' | awk -F',' '{print $1}'");
    metrics.push_back({"System", "Uptime", uptime, ""});

    std::string boot = run_cmd("who -b 2>/dev/null | awk '{print $3, $4}'");
    metrics.push_back({"System", "Boot Time", boot, ""});

    return metrics;
}

std::vector<ProfileMetric> Profiler::collect_packages() {
    std::vector<ProfileMetric> metrics;
    std::string count = run_cmd("dpkg -l 2>/dev/null | grep ^ii | wc -l");
    if (!count.empty() && count != "0") {
        metrics.push_back({"Packages", "Installed (dpkg)", count, ""});
    }
    return metrics;
}

std::vector<ProfileMetric> Profiler::collect_services() {
    std::vector<ProfileMetric> metrics;
    std::string active = run_cmd(
        "systemctl list-units --type=service --state=active 2>/dev/null | grep -c 'active running'");
    if (!active.empty()) {
        metrics.push_back({"Services", "Active", active, ""});
    }

    std::string sl_services = run_cmd(
        "systemctl list-units --type=service 2>/dev/null | grep -c straylight");
    if (!sl_services.empty()) {
        metrics.push_back({"Services", "StrayLight Active", sl_services, ""});
    }

    return metrics;
}

std::vector<ProfileMetric> Profiler::collect_thermal() {
    std::vector<ProfileMetric> metrics;

    // Try hwmon
    if (fs::exists("/sys/class/thermal/thermal_zone0/temp")) {
        std::string temp = run_cmd("cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null");
        if (!temp.empty()) {
            double celsius = std::stod(temp) / 1000.0;
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(1) << celsius;
            metrics.push_back({"Thermal", "CPU Temperature", oss.str(), "C"});
        }
    }

    // GPU temperature
    std::string gpu_temp = run_cmd(
        "nvidia-smi --query-gpu=temperature.gpu --format=csv,noheader 2>/dev/null | head -1");
    if (!gpu_temp.empty()) {
        metrics.push_back({"Thermal", "GPU Temperature", gpu_temp, "C"});
    }

    return metrics;
}

int Profiler::get_health_score() {
    // Try to connect to health daemon
    std::string score = run_cmd(
        "straylight-health-cli score 2>/dev/null");
    if (!score.empty()) {
        try { return std::stoi(score); } catch (...) {}
    }
    return -1;
}

int Profiler::get_security_score() {
    std::string score = run_cmd(
        "straylight-shield audit 2>/dev/null | grep 'Score:' | awk '{print $2}'");
    if (!score.empty()) {
        try { return std::stoi(score); } catch (...) {}
    }
    return -1;
}

SystemProfile Profiler::collect() {
    SystemProfile profile;
    profile.hostname = run_cmd("hostname 2>/dev/null");

    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    profile.timestamp = buf;

    // Collect all categories
    auto cpu = collect_cpu();
    auto mem = collect_memory();
    auto gpu = collect_gpu();
    auto disk = collect_disk();
    auto net = collect_network();
    auto kern = collect_kernel();
    auto boot = collect_boot();
    auto pkgs = collect_packages();
    auto svcs = collect_services();
    auto thermal = collect_thermal();

    profile.metrics.insert(profile.metrics.end(), cpu.begin(), cpu.end());
    profile.metrics.insert(profile.metrics.end(), mem.begin(), mem.end());
    profile.metrics.insert(profile.metrics.end(), gpu.begin(), gpu.end());
    profile.metrics.insert(profile.metrics.end(), disk.begin(), disk.end());
    profile.metrics.insert(profile.metrics.end(), net.begin(), net.end());
    profile.metrics.insert(profile.metrics.end(), kern.begin(), kern.end());
    profile.metrics.insert(profile.metrics.end(), boot.begin(), boot.end());
    profile.metrics.insert(profile.metrics.end(), pkgs.begin(), pkgs.end());
    profile.metrics.insert(profile.metrics.end(), svcs.begin(), svcs.end());
    profile.metrics.insert(profile.metrics.end(), thermal.begin(), thermal.end());

    profile.health_score = get_health_score();
    profile.security_score = get_security_score();

    return profile;
}

std::string Profiler::format_text(const SystemProfile& profile) {
    std::ostringstream out;
    out << "\033[1;36m=== StrayLight System Profile ===\033[0m\n";
    out << "Host: " << profile.hostname << "  |  " << profile.timestamp << "\n";

    if (profile.health_score >= 0) {
        std::string color = profile.health_score >= 70 ? "\033[32m" :
                            profile.health_score >= 40 ? "\033[33m" : "\033[31m";
        out << "Health: " << color << profile.health_score << "/100\033[0m";
    }
    if (profile.security_score >= 0) {
        std::string color = profile.security_score >= 70 ? "\033[32m" :
                            profile.security_score >= 40 ? "\033[33m" : "\033[31m";
        out << "  |  Security: " << color << profile.security_score << "/100\033[0m";
    }
    out << "\n\n";

    std::string current_cat;
    for (const auto& m : profile.metrics) {
        if (m.category != current_cat) {
            current_cat = m.category;
            out << "\033[1;33m" << current_cat << "\033[0m\n";
        }
        out << "  " << std::left << std::setw(25) << m.name << " ";
        out << m.value;
        if (!m.unit.empty()) out << " " << m.unit;
        out << "\n";
    }

    return out.str();
}

std::string Profiler::format_json(const SystemProfile& profile) {
    nlohmann::json j;
    j["hostname"] = profile.hostname;
    j["timestamp"] = profile.timestamp;
    j["health_score"] = profile.health_score;
    j["security_score"] = profile.security_score;
    j["metrics"] = nlohmann::json::object();

    for (const auto& m : profile.metrics) {
        if (!j["metrics"].contains(m.category)) {
            j["metrics"][m.category] = nlohmann::json::object();
        }
        nlohmann::json mv;
        mv["value"] = m.value;
        if (!m.unit.empty()) mv["unit"] = m.unit;
        j["metrics"][m.category][m.name] = mv;
    }

    return j.dump(2);
}

std::string Profiler::format_html(const SystemProfile& profile) {
    std::ostringstream out;
    out << "<!DOCTYPE html>\n<html><head>\n"
        << "<title>StrayLight System Profile - " << profile.hostname << "</title>\n"
        << "<style>\n"
        << "body { font-family: 'Segoe UI', sans-serif; background: #1a1a2e; color: #e0e0e0; "
        << "max-width: 800px; margin: 0 auto; padding: 20px; }\n"
        << "h1 { color: #00d4ff; border-bottom: 2px solid #00d4ff; padding-bottom: 10px; }\n"
        << "h2 { color: #ff6b6b; margin-top: 20px; }\n"
        << "table { width: 100%; border-collapse: collapse; margin-bottom: 15px; }\n"
        << "th { background: #16213e; text-align: left; padding: 8px; }\n"
        << "td { padding: 6px 8px; border-bottom: 1px solid #2a2a4a; }\n"
        << ".score { font-size: 1.5em; font-weight: bold; }\n"
        << ".good { color: #4caf50; } .warn { color: #ff9800; } .bad { color: #f44336; }\n"
        << "</style>\n</head><body>\n";

    out << "<h1>StrayLight System Profile</h1>\n";
    out << "<p>Host: <strong>" << profile.hostname << "</strong> &mdash; "
        << profile.timestamp << "</p>\n";

    if (profile.health_score >= 0 || profile.security_score >= 0) {
        out << "<div style='display:flex;gap:40px;margin:20px 0'>\n";
        if (profile.health_score >= 0) {
            std::string cls = profile.health_score >= 70 ? "good" :
                              profile.health_score >= 40 ? "warn" : "bad";
            out << "<div>Health: <span class='score " << cls << "'>"
                << profile.health_score << "/100</span></div>\n";
        }
        if (profile.security_score >= 0) {
            std::string cls = profile.security_score >= 70 ? "good" :
                              profile.security_score >= 40 ? "warn" : "bad";
            out << "<div>Security: <span class='score " << cls << "'>"
                << profile.security_score << "/100</span></div>\n";
        }
        out << "</div>\n";
    }

    std::string current_cat;
    for (const auto& m : profile.metrics) {
        if (m.category != current_cat) {
            if (!current_cat.empty()) out << "</table>\n";
            current_cat = m.category;
            out << "<h2>" << current_cat << "</h2>\n"
                << "<table><tr><th>Metric</th><th>Value</th></tr>\n";
        }
        out << "<tr><td>" << m.name << "</td><td>" << m.value;
        if (!m.unit.empty()) out << " " << m.unit;
        out << "</td></tr>\n";
    }
    if (!current_cat.empty()) out << "</table>\n";

    out << "<footer style='margin-top:30px;color:#666;font-size:0.8em'>"
        << "Generated by StrayLight OS Profiler</footer>\n"
        << "</body></html>\n";

    return out.str();
}

ProfileComparison Profiler::compare(const SystemProfile& a, const SystemProfile& b) {
    ProfileComparison result;
    result.old_health = a.health_score;
    result.new_health = b.health_score;

    // Index metrics from profile A
    std::map<std::string, const ProfileMetric*> a_map;
    for (const auto& m : a.metrics) {
        a_map[m.category + "::" + m.name] = &m;
    }

    // Compare with profile B
    for (const auto& m : b.metrics) {
        std::string key = m.category + "::" + m.name;
        auto it = a_map.find(key);
        if (it != a_map.end()) {
            if (it->second->value != m.value) {
                result.changes.push_back({
                    m.category, m.name, it->second->value, m.value
                });
            }
            a_map.erase(it);
        } else {
            result.changes.push_back({m.category, m.name, "(new)", m.value});
        }
    }

    // Metrics in A but not in B
    for (const auto& [key, metric] : a_map) {
        result.changes.push_back({
            metric->category, metric->name, metric->value, "(removed)"
        });
    }

    return result;
}

Result<SystemProfile, std::string> Profiler::load(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs) {
        return Result<SystemProfile, std::string>::error("Cannot open: " + path);
    }

    try {
        nlohmann::json j;
        ifs >> j;

        SystemProfile profile;
        profile.hostname = j.value("hostname", "");
        profile.timestamp = j.value("timestamp", "");
        profile.health_score = j.value("health_score", -1);
        profile.security_score = j.value("security_score", -1);

        if (j.contains("metrics") && j["metrics"].is_object()) {
            for (auto& [cat, cat_obj] : j["metrics"].items()) {
                if (!cat_obj.is_object()) continue;
                for (auto& [name, val_obj] : cat_obj.items()) {
                    ProfileMetric m;
                    m.category = cat;
                    m.name = name;
                    if (val_obj.is_object()) {
                        m.value = val_obj.value("value", "");
                        m.unit = val_obj.value("unit", "");
                    } else {
                        m.value = val_obj.get<std::string>();
                    }
                    profile.metrics.push_back(std::move(m));
                }
            }
        }

        return Result<SystemProfile, std::string>::ok(std::move(profile));
    } catch (const std::exception& e) {
        return Result<SystemProfile, std::string>::error(
            std::string("Parse error: ") + e.what());
    }
}

Result<void, std::string> Profiler::save(const SystemProfile& profile,
                                          const std::string& path) {
    std::string json = format_json(profile);
    std::ofstream ofs(path);
    if (!ofs) {
        return Result<void, std::string>::error("Cannot write: " + path);
    }
    ofs << json << "\n";
    return Result<void, std::string>::ok();
}

} // namespace straylight
