// tools/service/service_manager.cpp
#include "service_manager.h"

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace straylight {

namespace fs = std::filesystem;

namespace {
std::string run_cmd(const std::string& cmd) {
    std::array<char, 4096> buf;
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) return "";
    while (fgets(buf.data(), static_cast<int>(buf.size()), pipe.get())) result += buf.data();
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) result.pop_back();
    return result;
}
int run_rc(const std::string& cmd) { return WEXITSTATUS(std::system(cmd.c_str())); }
} // namespace

std::string ServiceManager::systemctl(const std::string& args) const {
    return run_cmd("systemctl " + args + " 2>/dev/null");
}
std::string ServiceManager::journalctl(const std::string& args) const {
    return run_cmd("journalctl " + args + " 2>/dev/null");
}
bool ServiceManager::is_straylight_service(const std::string& name) const {
    return name.find("straylight") != std::string::npos;
}
uint64_t ServiceManager::parse_memory(const std::string& s) const {
    if (s.empty()) return 0;
    try {
        double v = std::stod(s);
        char c = s.back();
        if (c == 'K' || c == 'k') return static_cast<uint64_t>(v * 1024);
        if (c == 'M' || c == 'm') return static_cast<uint64_t>(v * 1048576);
        if (c == 'G' || c == 'g') return static_cast<uint64_t>(v * 1073741824);
        return static_cast<uint64_t>(v);
    } catch (...) { return 0; }
}

std::vector<ServiceInfo> ServiceManager::list(const std::string& filter) const {
    std::vector<ServiceInfo> svcs;
    std::string out = run_cmd("systemctl list-units --type=service --all --no-pager --no-legend --plain 2>/dev/null");
    std::istringstream iss(out);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.empty()) continue;
        std::istringstream ls(line);
        std::string unit, load, active, sub;
        ls >> unit >> load >> active >> sub;
        std::string desc; std::getline(ls, desc);
        while (!desc.empty() && desc.front() == ' ') desc.erase(desc.begin());
        std::string name = unit;
        if (name.size() > 8 && name.substr(name.size() - 8) == ".service")
            name = name.substr(0, name.size() - 8);
        bool is_sl = is_straylight_service(name);
        if (filter == "straylight" && !is_sl) continue;
        if (filter == "system" && is_sl) continue;
        svcs.push_back({name, desc, active, sub, is_sl, 0, 0.0, "", ""});
    }
    return svcs;
}

Result<ServiceInfo, std::string> ServiceManager::status(const std::string& name) const {
    std::string unit = name + (name.find(".service") == std::string::npos ? ".service" : "");
    std::string active = systemctl("show -p ActiveState --value " + unit);
    if (active.empty()) return Result<ServiceInfo, std::string>::error("Not found: " + name);
    ServiceInfo info;
    info.name = name;
    info.state = active;
    info.sub_state = systemctl("show -p SubState --value " + unit);
    info.description = systemctl("show -p Description --value " + unit);
    info.main_pid = systemctl("show -p MainPID --value " + unit);
    info.started_at = systemctl("show -p ActiveEnterTimestamp --value " + unit);
    info.is_straylight = is_straylight_service(name);
    std::string mem = systemctl("show -p MemoryCurrent --value " + unit);
    if (!mem.empty() && mem != "[not set]") try { info.memory_bytes = std::stoull(mem); } catch (...) {}
    return Result<ServiceInfo, std::string>::ok(info);
}

Result<void, std::string> ServiceManager::start(const std::string& name) {
    std::string u = name + (name.find(".service") == std::string::npos ? ".service" : "");
    return run_rc("systemctl start " + u + " 2>&1") == 0
        ? Result<void, std::string>::ok()
        : Result<void, std::string>::error("Failed to start " + name);
}
Result<void, std::string> ServiceManager::stop(const std::string& name) {
    std::string u = name + (name.find(".service") == std::string::npos ? ".service" : "");
    return run_rc("systemctl stop " + u + " 2>&1") == 0
        ? Result<void, std::string>::ok()
        : Result<void, std::string>::error("Failed to stop " + name);
}
Result<void, std::string> ServiceManager::restart(const std::string& name) {
    std::string u = name + (name.find(".service") == std::string::npos ? ".service" : "");
    return run_rc("systemctl restart " + u + " 2>&1") == 0
        ? Result<void, std::string>::ok()
        : Result<void, std::string>::error("Failed to restart " + name);
}

Result<std::string, std::string> ServiceManager::logs(const std::string& name, int lines, bool /*follow*/) const {
    std::string u = name + (name.find(".service") == std::string::npos ? ".service" : "");
    std::string out = journalctl("-u " + u + " -n " + std::to_string(lines) + " --no-pager");
    if (out.empty()) return Result<std::string, std::string>::error("No logs for " + name);
    return Result<std::string, std::string>::ok(out);
}

std::vector<ServiceDep> ServiceManager::dependencies(const std::string& name) const {
    std::vector<ServiceDep> deps;
    std::string u = name + (name.find(".service") == std::string::npos ? ".service" : "");
    for (const auto& [prop, type] : std::vector<std::pair<std::string,std::string>>{
            {"Requires","Requires"},{"Wants","Wants"},{"After","After"},{"Before","Before"}}) {
        std::string val = systemctl("show -p " + prop + " --value " + u);
        if (val.empty()) continue;
        std::istringstream iss(val); std::string dep;
        while (iss >> dep) { if (dep != u) deps.push_back({dep, type}); }
    }
    return deps;
}

Result<ServiceResources, std::string> ServiceManager::resources(const std::string& name) const {
    std::string u = name + (name.find(".service") == std::string::npos ? ".service" : "");
    ServiceResources r; r.name = name;
    auto try_ull = [&](const std::string& prop) -> uint64_t {
        std::string v = systemctl("show -p " + prop + " --value " + u);
        if (v.empty() || v == "[not set]" || v == "infinity") return 0;
        try { return std::stoull(v); } catch (...) { return 0; }
    };
    r.memory_current = try_ull("MemoryCurrent");
    r.memory_peak = try_ull("MemoryPeak");
    r.memory_limit = try_ull("MemoryMax");
    std::string tasks = systemctl("show -p TasksCurrent --value " + u);
    if (!tasks.empty() && tasks != "[not set]") try { r.task_count = std::stoi(tasks); } catch (...) {}
    return Result<ServiceResources, std::string>::ok(r);
}

Result<void, std::string> ServiceManager::create_service(const std::string& name, const std::string& description) {
    std::string sn = "straylight-" + name;
    fs::path up = fs::path("/etc/systemd/system") / (sn + ".service");
    std::string desc = description.empty() ? "StrayLight " + name + " daemon" : description;
    std::ofstream ofs(up);
    if (!ofs) return Result<void, std::string>::error("Cannot write (run as root): " + up.string());
    ofs << "[Unit]\nDescription=" << desc << "\nAfter=network.target\n\n[Service]\nType=simple\n"
        << "ExecStart=/usr/bin/" << sn << "\nRestart=on-failure\nRestartSec=5\n"
        << "StandardOutput=journal\nStandardError=journal\n"
        << "Environment=SL_CONFIG_DIR=/etc/straylight\nRuntimeDirectory=straylight\n"
        << "RuntimeDirectoryPreserve=yes\n\n[Install]\nWantedBy=multi-user.target\n";
    std::error_code ec; fs::create_directories("/etc/straylight", ec);
    fs::path cp = fs::path("/etc/straylight") / (name + ".conf");
    if (!fs::exists(cp)) {
        std::ofstream cf(cp);
        if (cf) cf << "{\n    \"tick_interval_seconds\": 60,\n    \"ipc\": {\n"
                   << "        \"socket_path\": \"/run/straylight/" << name << ".sock\"\n    }\n}\n";
    }
    return Result<void, std::string>::ok();
}

Result<void, std::string> ServiceManager::enable(const std::string& name) {
    std::string u = name + (name.find(".service") == std::string::npos ? ".service" : "");
    return run_rc("systemctl enable " + u + " 2>&1") == 0
        ? Result<void, std::string>::ok()
        : Result<void, std::string>::error("Failed to enable " + name);
}
Result<void, std::string> ServiceManager::disable(const std::string& name) {
    std::string u = name + (name.find(".service") == std::string::npos ? ".service" : "");
    return run_rc("systemctl disable " + u + " 2>&1") == 0
        ? Result<void, std::string>::ok()
        : Result<void, std::string>::error("Failed to disable " + name);
}

} // namespace straylight
