// tools/port/port_manager.cpp
// Full port manager implementation for StrayLight OS.

#include "port_manager.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace straylight {

PortManager::PortManager() {
    // Ensure config directory exists
    std::string dir = fs::path(config_path()).parent_path().string();
    fs::create_directories(dir);
}

PortManager::~PortManager() = default;

std::string PortManager::config_path() const {
    return "/etc/straylight/ports.conf";
}

Result<std::string, std::string> PortManager::run_cmd(const std::string& cmd) const {
    std::array<char, 8192> buffer{};
    std::string output;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return Result<std::string, std::string>::error(
            "popen failed: " + std::string(strerror(errno)));
    }
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }
    int rc = pclose(pipe);
    if (rc != 0 && output.empty()) {
        return Result<std::string, std::string>::error("command failed: " + cmd);
    }
    return Result<std::string, std::string>::ok(output);
}

// ---------------------------------------------------------------------------
// /proc/net parsing helpers
// ---------------------------------------------------------------------------

std::string PortManager::hex_to_ip(const std::string& hex_ip) const {
    if (hex_ip.size() == 8) {
        // IPv4: stored in little-endian hex
        unsigned long addr = std::stoul(hex_ip, nullptr, 16);
        return std::to_string(addr & 0xFF) + "." +
               std::to_string((addr >> 8) & 0xFF) + "." +
               std::to_string((addr >> 16) & 0xFF) + "." +
               std::to_string((addr >> 24) & 0xFF);
    }
    if (hex_ip.size() == 32) {
        // IPv6: display as ::
        std::string result;
        for (int i = 0; i < 32; i += 8) {
            // Each 8-char group is a 32-bit word in network order
            std::string word = hex_ip.substr(i, 8);
            // Reverse bytes within each 32-bit word
            std::string reversed;
            for (int j = 6; j >= 0; j -= 2) {
                reversed += word.substr(j, 2);
            }
            if (!result.empty()) result += ":";
            // Remove leading zeros
            size_t nz = reversed.find_first_not_of('0');
            if (nz == std::string::npos) result += "0";
            else result += reversed.substr(nz);
        }
        return result;
    }
    return hex_ip;
}

uint16_t PortManager::hex_to_port(const std::string& hex_port) const {
    return static_cast<uint16_t>(std::stoul(hex_port, nullptr, 16));
}

std::string PortManager::pid_to_name(int pid) const {
    std::string path = "/proc/" + std::to_string(pid) + "/comm";
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::string name;
    std::getline(f, name);
    return name;
}

std::string PortManager::pid_to_user(int pid) const {
    std::string path = "/proc/" + std::to_string(pid) + "/status";
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("Uid:", 0) == 0) {
            std::istringstream ss(line.substr(4));
            int uid = 0;
            ss >> uid;
            // Resolve UID to username
            auto res = run_cmd("id -nu " + std::to_string(uid) + " 2>/dev/null");
            if (res.has_value()) {
                std::string user = res.value();
                if (!user.empty() && user.back() == '\n') user.pop_back();
                return user;
            }
            return std::to_string(uid);
        }
    }
    return "";
}

std::vector<PortEntry> PortManager::parse_proc_net(const std::string& proto) const {
    std::vector<PortEntry> entries;

    std::string path = "/proc/net/" + proto;
    std::ifstream f(path);
    if (!f.is_open()) return entries;

    std::string line;
    std::getline(f, line); // Skip header

    // Build inode-to-PID mapping
    std::map<std::string, int> inode_to_pid;
    if (fs::exists("/proc")) {
        for (const auto& proc_entry : fs::directory_iterator("/proc")) {
            std::string name = proc_entry.path().filename().string();
            int pid = 0;
            try { pid = std::stoi(name); } catch (...) { continue; }

            std::string fd_dir = "/proc/" + name + "/fd";
            if (!fs::exists(fd_dir)) continue;

            try {
                for (const auto& fd_entry : fs::directory_iterator(fd_dir)) {
                    auto link = fs::read_symlink(fd_entry.path());
                    std::string link_str = link.string();
                    if (link_str.rfind("socket:[", 0) == 0) {
                        std::string inode = link_str.substr(8, link_str.size() - 9);
                        inode_to_pid[inode] = pid;
                    }
                }
            } catch (...) {
                // Permission denied for some processes
            }
        }
    }

    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string idx, local, remote, st, txrx, tr, retrnsmt, uid_field, timeout, inode;

        ss >> idx >> local >> remote >> st >> txrx >> tr >> retrnsmt >> uid_field >> timeout >> inode;

        if (local.empty()) continue;

        PortEntry entry;
        entry.protocol = proto;

        // Parse local address
        auto colon = local.rfind(':');
        if (colon != std::string::npos) {
            entry.local_addr = hex_to_ip(local.substr(0, colon));
            entry.port = hex_to_port(local.substr(colon + 1));
        }

        // Parse remote address
        colon = remote.rfind(':');
        if (colon != std::string::npos) {
            entry.remote_addr = hex_to_ip(remote.substr(0, colon)) + ":" +
                                std::to_string(hex_to_port(remote.substr(colon + 1)));
        }

        // State
        int state_int = std::stoi(st, nullptr, 16);
        switch (state_int) {
            case 1: entry.state = "ESTABLISHED"; break;
            case 2: entry.state = "SYN_SENT"; break;
            case 3: entry.state = "SYN_RECV"; break;
            case 4: entry.state = "FIN_WAIT1"; break;
            case 5: entry.state = "FIN_WAIT2"; break;
            case 6: entry.state = "TIME_WAIT"; break;
            case 7: entry.state = "CLOSE"; break;
            case 8: entry.state = "CLOSE_WAIT"; break;
            case 9: entry.state = "LAST_ACK"; break;
            case 10: entry.state = "LISTEN"; break;
            case 11: entry.state = "CLOSING"; break;
            default: entry.state = "UNKNOWN"; break;
        }

        // Map inode to PID
        auto it = inode_to_pid.find(inode);
        if (it != inode_to_pid.end()) {
            entry.pid = it->second;
            entry.process_name = pid_to_name(entry.pid);
            entry.user = pid_to_user(entry.pid);
        }

        entries.push_back(entry);
    }

    return entries;
}

// ---------------------------------------------------------------------------
// Reservations persistence
// ---------------------------------------------------------------------------

std::map<uint16_t, PortReservation> PortManager::load_reservations() const {
    std::map<uint16_t, PortReservation> reservations;
    std::string path = config_path();

    std::ifstream f(path);
    if (!f.is_open()) return reservations;

    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;

        // Format: port|protocol|service|description
        std::istringstream ss(line);
        std::string field;
        PortReservation res;

        if (!std::getline(ss, field, '|')) continue;
        try { res.port = static_cast<uint16_t>(std::stoi(field)); } catch (...) { continue; }

        if (std::getline(ss, field, '|')) res.protocol = field;
        if (std::getline(ss, field, '|')) res.service_name = field;
        if (std::getline(ss, field, '|')) res.description = field;

        reservations[res.port] = res;
    }

    return reservations;
}

void PortManager::save_reservations(const std::map<uint16_t, PortReservation>& reservations) const {
    std::string path = config_path();

    // Ensure directory exists
    fs::create_directories(fs::path(path).parent_path());

    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open()) return;

    f << "# StrayLight OS Port Reservations\n"
      << "# Format: port|protocol|service|description\n";

    for (const auto& [port, res] : reservations) {
        f << res.port << "|" << res.protocol << "|"
          << res.service_name << "|" << res.description << "\n";
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

Result<std::vector<PortEntry>, std::string> PortManager::list(bool listening_only) const {
    std::vector<PortEntry> all;

    // Try ss/netstat first for a simpler approach
    auto ss_res = run_cmd("ss -tulnp 2>/dev/null");
    if (ss_res.has_value() && !ss_res.value().empty()) {
        std::istringstream stream(ss_res.value());
        std::string line;
        std::getline(stream, line); // Skip header

        while (std::getline(stream, line)) {
            if (line.empty()) continue;
            std::istringstream ls(line);
            std::string netid, state, recvq, sendq, local, peer;
            ls >> netid >> state >> recvq >> sendq >> local >> peer;

            PortEntry entry;
            entry.protocol = netid;
            entry.state = state;

            auto colon = local.rfind(':');
            if (colon != std::string::npos) {
                entry.local_addr = local.substr(0, colon);
                try { entry.port = static_cast<uint16_t>(std::stoi(local.substr(colon + 1))); }
                catch (...) {}
            }
            entry.remote_addr = peer;

            // Parse process info: "users:(("sshd",pid=1234,fd=3))"
            std::string rest;
            std::getline(ls, rest);
            std::regex pid_re(R"(pid=(\d+))");
            std::regex name_re(R"(\("([^"]+)")");
            std::smatch m;
            if (std::regex_search(rest, m, pid_re)) {
                entry.pid = std::stoi(m[1].str());
            }
            if (std::regex_search(rest, m, name_re)) {
                entry.process_name = m[1].str();
            }

            if (listening_only && state != "LISTEN") continue;
            all.push_back(entry);
        }
    } else {
        // Fallback: parse /proc/net directly
        for (const auto& proto : {"tcp", "tcp6", "udp", "udp6"}) {
            auto entries = parse_proc_net(proto);
            for (auto& e : entries) {
                if (listening_only && e.state != "LISTEN") continue;
                all.push_back(std::move(e));
            }
        }
    }

    // Sort by port
    std::sort(all.begin(), all.end(),
              [](const auto& a, const auto& b) { return a.port < b.port; });

    return Result<std::vector<PortEntry>, std::string>::ok(all);
}

Result<PortEntry, std::string> PortManager::who(uint16_t port) const {
    auto list_res = list(false);
    if (!list_res.has_value()) {
        return Result<PortEntry, std::string>::error(list_res.error());
    }

    for (const auto& e : list_res.value()) {
        if (e.port == port) {
            return Result<PortEntry, std::string>::ok(e);
        }
    }

    return Result<PortEntry, std::string>::error(
        "no process found on port " + std::to_string(port));
}

Result<void, std::string> PortManager::kill(uint16_t port, int signal) const {
    auto who_res = who(port);
    if (!who_res.has_value()) {
        return Result<void, std::string>::error(who_res.error());
    }

    const auto& entry = who_res.value();
    if (entry.pid <= 0) {
        return Result<void, std::string>::error("could not determine PID for port " +
                                                  std::to_string(port));
    }

    auto res = run_cmd("kill -" + std::to_string(signal) + " " +
                        std::to_string(entry.pid) + " 2>&1");
    if (!res.has_value()) {
        return Result<void, std::string>::error("failed to kill PID " +
                                                  std::to_string(entry.pid) + ": " + res.error());
    }

    return Result<void, std::string>::ok();
}

Result<void, std::string> PortManager::reserve(uint16_t port, const std::string& service,
                                                 const std::string& protocol,
                                                 const std::string& description) {
    auto reservations = load_reservations();

    PortReservation res;
    res.port = port;
    res.service_name = service;
    res.protocol = protocol;
    res.description = description;

    reservations[port] = res;
    save_reservations(reservations);

    return Result<void, std::string>::ok();
}

Result<void, std::string> PortManager::unreserve(uint16_t port) {
    auto reservations = load_reservations();
    auto it = reservations.find(port);
    if (it == reservations.end()) {
        return Result<void, std::string>::error(
            "port " + std::to_string(port) + " is not reserved");
    }
    reservations.erase(it);
    save_reservations(reservations);
    return Result<void, std::string>::ok();
}

Result<std::vector<PortConflict>, std::string> PortManager::conflicts() const {
    auto reservations = load_reservations();
    auto ports_res = list(true);
    if (!ports_res.has_value()) {
        return Result<std::vector<PortConflict>, std::string>::error(ports_res.error());
    }

    std::vector<PortConflict> conflicts;

    for (const auto& [port, reservation] : reservations) {
        for (const auto& entry : ports_res.value()) {
            if (entry.port == port && !entry.process_name.empty()) {
                // Check if the process matches the reservation
                if (entry.process_name.find(reservation.service_name) == std::string::npos) {
                    PortConflict conflict;
                    conflict.port = port;
                    conflict.reserved_for = reservation.service_name;
                    conflict.actual_process = entry.process_name;
                    conflict.actual_pid = entry.pid;
                    conflicts.push_back(conflict);
                }
            }
        }
    }

    return Result<std::vector<PortConflict>, std::string>::ok(conflicts);
}

Result<std::vector<uint16_t>, std::string> PortManager::free_ports(uint16_t start,
                                                                     uint16_t end) const {
    auto ports_res = list(true);
    std::set<uint16_t> used;

    if (ports_res.has_value()) {
        for (const auto& e : ports_res.value()) {
            used.insert(e.port);
        }
    }

    auto reservations = load_reservations();
    for (const auto& [port, _] : reservations) {
        used.insert(port);
    }

    std::vector<uint16_t> free;
    for (uint16_t p = start; p <= end && p >= start; ++p) {
        if (used.find(p) == used.end()) {
            free.push_back(p);
        }
    }

    return Result<std::vector<uint16_t>, std::string>::ok(free);
}

Result<std::vector<PortReservation>, std::string> PortManager::reservations() const {
    auto res_map = load_reservations();
    std::vector<PortReservation> result;
    for (const auto& [_, res] : res_map) {
        result.push_back(res);
    }
    return Result<std::vector<PortReservation>, std::string>::ok(result);
}

} // namespace straylight
