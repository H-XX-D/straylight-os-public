// tools/proc/proc_inspector.cpp
// Full process inspector implementation for StrayLight OS.

#include "proc_inspector.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
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

ProcInspector::ProcInspector() = default;
ProcInspector::~ProcInspector() = default;

Result<std::string, std::string> ProcInspector::read_proc_file(int pid,
                                                                  const std::string& file) const {
    std::string path = "/proc/" + std::to_string(pid) + "/" + file;
    std::ifstream f(path);
    if (!f.is_open()) {
        return Result<std::string, std::string>::error("cannot read " + path);
    }
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    return Result<std::string, std::string>::ok(content);
}

Result<std::string, std::string> ProcInspector::run_cmd(const std::string& cmd) const {
    std::array<char, 8192> buffer{};
    std::string output;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return Result<std::string, std::string>::error("popen failed");
    }
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }
    pclose(pipe);
    return Result<std::string, std::string>::ok(output);
}

std::string ProcInspector::resolve_state(char state_char) const {
    switch (state_char) {
        case 'R': return "running";
        case 'S': return "sleeping";
        case 'D': return "disk sleep";
        case 'Z': return "zombie";
        case 'T': return "stopped";
        case 't': return "tracing stop";
        case 'X': return "dead";
        case 'I': return "idle";
        default: return std::string(1, state_char);
    }
}

std::string ProcInspector::hex_to_ip(const std::string& hex_ip) const {
    if (hex_ip.size() == 8) {
        unsigned long addr = std::stoul(hex_ip, nullptr, 16);
        return std::to_string(addr & 0xFF) + "." +
               std::to_string((addr >> 8) & 0xFF) + "." +
               std::to_string((addr >> 16) & 0xFF) + "." +
               std::to_string((addr >> 24) & 0xFF);
    }
    return hex_ip;
}

uint16_t ProcInspector::hex_to_port(const std::string& hex_port) const {
    return static_cast<uint16_t>(std::stoul(hex_port, nullptr, 16));
}

// ---------------------------------------------------------------------------
// Info
// ---------------------------------------------------------------------------

Result<ProcInfo, std::string> ProcInspector::info(int pid) const {
    ProcInfo proc;
    proc.pid = pid;

    // /proc/PID/status
    auto status = read_proc_file(pid, "status");
    if (!status.has_value()) {
        return Result<ProcInfo, std::string>::error("process " + std::to_string(pid) + " not found");
    }

    std::istringstream stream(status.value());
    std::string line;
    while (std::getline(stream, line)) {
        if (line.rfind("Name:", 0) == 0) {
            proc.name = line.substr(6);
            auto p = proc.name.find_first_not_of(" \t");
            if (p != std::string::npos) proc.name = proc.name.substr(p);
        } else if (line.rfind("State:", 0) == 0) {
            auto p = line.find_first_not_of(" \t", 6);
            if (p != std::string::npos) {
                proc.state_char = std::string(1, line[p]);
                proc.state = resolve_state(line[p]);
            }
        } else if (line.rfind("PPid:", 0) == 0) {
            try { proc.ppid = std::stoi(line.substr(5)); } catch (...) {}
        } else if (line.rfind("Threads:", 0) == 0) {
            try { proc.threads = std::stoi(line.substr(8)); } catch (...) {}
        } else if (line.rfind("Uid:", 0) == 0) {
            std::istringstream us(line.substr(4));
            us >> proc.uid;
        } else if (line.rfind("Gid:", 0) == 0) {
            std::istringstream gs(line.substr(4));
            gs >> proc.gid;
        } else if (line.rfind("VmSize:", 0) == 0) {
            std::regex num_re(R"((\d+)\s+kB)");
            std::smatch m;
            if (std::regex_search(line, m, num_re)) proc.vm_size = std::stoull(m[1].str()) * 1024;
        } else if (line.rfind("VmRSS:", 0) == 0) {
            std::regex num_re(R"((\d+)\s+kB)");
            std::smatch m;
            if (std::regex_search(line, m, num_re)) proc.vm_rss = std::stoull(m[1].str()) * 1024;
        } else if (line.rfind("RssAnon:", 0) == 0 || line.rfind("RssShmem:", 0) == 0) {
            // Skip
        }
    }

    // Resolve username
    auto user_res = run_cmd("id -nu " + std::to_string(proc.uid) + " 2>/dev/null");
    if (user_res.has_value()) {
        proc.user = user_res.value();
        if (!proc.user.empty() && proc.user.back() == '\n') proc.user.pop_back();
    }

    // /proc/PID/cmdline
    auto cmdline = read_proc_file(pid, "cmdline");
    if (cmdline.has_value()) {
        std::string cmd = cmdline.value();
        for (auto& c : cmd) { if (c == '\0') c = ' '; }
        if (!cmd.empty() && cmd.back() == ' ') cmd.pop_back();
        proc.cmdline = cmd;
    }

    // /proc/PID/exe
    try {
        proc.exe = fs::read_symlink("/proc/" + std::to_string(pid) + "/exe").string();
    } catch (...) {}

    // /proc/PID/cwd
    try {
        proc.cwd = fs::read_symlink("/proc/" + std::to_string(pid) + "/cwd").string();
    } catch (...) {}

    // /proc/PID/cgroup
    auto cgroup = read_proc_file(pid, "cgroup");
    if (cgroup.has_value()) {
        std::string cg = cgroup.value();
        // v2 format: "0::/<cgroup>"
        auto pos = cg.find("::");
        if (pos != std::string::npos) {
            proc.cgroup = cg.substr(pos + 2);
            if (!proc.cgroup.empty() && proc.cgroup.back() == '\n') proc.cgroup.pop_back();
        }
    }

    // Nice value from /proc/PID/stat (field 19)
    auto stat = read_proc_file(pid, "stat");
    if (stat.has_value()) {
        // Find the closing paren (to skip command name which may have spaces)
        auto close_paren = stat.value().rfind(')');
        if (close_paren != std::string::npos) {
            std::istringstream ss(stat.value().substr(close_paren + 2));
            std::string field;
            // Fields after (comm): state(1) ppid(2) pgrp(3) session(4) tty(5)
            // tpgid(6) flags(7) minflt(8) cminflt(9) majflt(10) cmajflt(11)
            // utime(12) stime(13) cutime(14) cstime(15) priority(16) nice(17)
            for (int i = 1; i <= 17 && ss >> field; ++i) {
                if (i == 17) {
                    try { proc.nice = std::stoi(field); } catch (...) {}
                }
            }
        }
    }

    return Result<ProcInfo, std::string>::ok(proc);
}

// ---------------------------------------------------------------------------
// Tree
// ---------------------------------------------------------------------------

ProcTreeNode ProcInspector::build_tree(int pid,
                                         const std::map<int, std::vector<int>>& children_map,
                                         const std::map<int, std::string>& names,
                                         const std::map<int, std::string>& states) const {
    ProcTreeNode node;
    node.pid = pid;
    auto nit = names.find(pid);
    node.name = (nit != names.end()) ? nit->second : "?";
    auto sit = states.find(pid);
    node.state_char = (sit != states.end()) ? sit->second : "?";

    auto cit = children_map.find(pid);
    if (cit != children_map.end()) {
        for (int child_pid : cit->second) {
            node.children.push_back(build_tree(child_pid, children_map, names, states));
        }
    }

    return node;
}

Result<ProcTreeNode, std::string> ProcInspector::tree() const {
    std::map<int, int> parent_map;
    std::map<int, std::vector<int>> children_map;
    std::map<int, std::string> names;
    std::map<int, std::string> states;

    for (const auto& entry : fs::directory_iterator("/proc")) {
        std::string name = entry.path().filename().string();
        int pid = 0;
        try { pid = std::stoi(name); } catch (...) { continue; }

        auto status = read_proc_file(pid, "status");
        if (!status.has_value()) continue;

        std::istringstream stream(status.value());
        std::string line;
        int ppid = 0;
        std::string proc_name, state;

        while (std::getline(stream, line)) {
            if (line.rfind("Name:", 0) == 0) {
                proc_name = line.substr(6);
                auto p = proc_name.find_first_not_of(" \t");
                if (p != std::string::npos) proc_name = proc_name.substr(p);
            } else if (line.rfind("PPid:", 0) == 0) {
                try { ppid = std::stoi(line.substr(5)); } catch (...) {}
            } else if (line.rfind("State:", 0) == 0) {
                auto p = line.find_first_not_of(" \t", 6);
                if (p != std::string::npos) state = std::string(1, line[p]);
            }
        }

        parent_map[pid] = ppid;
        children_map[ppid].push_back(pid);
        names[pid] = proc_name;
        states[pid] = state;
    }

    // Sort children by PID
    for (auto& [_, children] : children_map) {
        std::sort(children.begin(), children.end());
    }

    // Build from PID 1 (init)
    ProcTreeNode root;
    if (children_map.count(0)) {
        // PID 0's children are kernel threads + init
        root = build_tree(1, children_map, names, states);
    } else {
        root.pid = 1;
        root.name = "init";
    }

    return Result<ProcTreeNode, std::string>::ok(root);
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

Result<std::vector<OpenFile>, std::string> ProcInspector::files(int pid) const {
    std::vector<OpenFile> fds;
    std::string fd_dir = "/proc/" + std::to_string(pid) + "/fd";

    if (!fs::exists(fd_dir)) {
        return Result<std::vector<OpenFile>, std::string>::error(
            "cannot access " + fd_dir);
    }

    try {
        for (const auto& entry : fs::directory_iterator(fd_dir)) {
            OpenFile file;
            try { file.fd = std::stoi(entry.path().filename().string()); } catch (...) { continue; }

            try {
                std::string link = fs::read_symlink(entry.path()).string();
                file.path = link;

                if (link.rfind("socket:", 0) == 0) file.type = "socket";
                else if (link.rfind("pipe:", 0) == 0) file.type = "pipe";
                else if (link.rfind("anon_inode:", 0) == 0) file.type = "anon_inode";
                else if (link.find("/dev/") == 0) file.type = "device";
                else file.type = "regular";
            } catch (...) {
                file.type = "unknown";
            }

            // Read mode from fdinfo
            std::string fdinfo_path = "/proc/" + std::to_string(pid) + "/fdinfo/" +
                                       std::to_string(file.fd);
            std::ifstream fdinfo(fdinfo_path);
            if (fdinfo.is_open()) {
                std::string line;
                while (std::getline(fdinfo, line)) {
                    if (line.rfind("flags:", 0) == 0) {
                        std::string flags_str = line.substr(6);
                        auto p = flags_str.find_first_not_of(" \t");
                        if (p != std::string::npos) flags_str = flags_str.substr(p);
                        unsigned long flags = std::stoul(flags_str, nullptr, 8);
                        int accmode = flags & 3;
                        if (accmode == 0) file.mode = "r";
                        else if (accmode == 1) file.mode = "w";
                        else if (accmode == 2) file.mode = "rw";
                    }
                }
            }

            fds.push_back(file);
        }
    } catch (...) {
        return Result<std::vector<OpenFile>, std::string>::error(
            "permission denied for PID " + std::to_string(pid));
    }

    std::sort(fds.begin(), fds.end(),
              [](const auto& a, const auto& b) { return a.fd < b.fd; });

    return Result<std::vector<OpenFile>, std::string>::ok(fds);
}

// ---------------------------------------------------------------------------
// Memory
// ---------------------------------------------------------------------------

Result<std::vector<MemRegion>, std::string> ProcInspector::mem(int pid) const {
    auto maps = read_proc_file(pid, "maps");
    if (!maps.has_value()) {
        return Result<std::vector<MemRegion>, std::string>::error(maps.error());
    }

    // Also try smaps for detailed memory
    auto smaps = read_proc_file(pid, "smaps");

    std::vector<MemRegion> regions;
    std::istringstream stream(maps.value());
    std::string line;

    while (std::getline(stream, line)) {
        MemRegion region;
        std::istringstream ls(line);

        ls >> region.address >> region.perms;
        std::string offset_str, dev_str, inode_str;
        ls >> offset_str >> dev_str >> inode_str;

        try { region.offset = std::stoull(offset_str, nullptr, 16); } catch (...) {}
        region.device = dev_str;
        try { region.inode = std::stoull(inode_str); } catch (...) {}

        // Rest is pathname
        std::getline(ls, region.pathname);
        auto p = region.pathname.find_first_not_of(" \t");
        if (p != std::string::npos) region.pathname = region.pathname.substr(p);

        // Calculate size from address range
        auto dash = region.address.find('-');
        if (dash != std::string::npos) {
            try {
                uint64_t start = std::stoull(region.address.substr(0, dash), nullptr, 16);
                uint64_t end = std::stoull(region.address.substr(dash + 1), nullptr, 16);
                region.size = end - start;
            } catch (...) {}
        }

        regions.push_back(region);
    }

    return Result<std::vector<MemRegion>, std::string>::ok(regions);
}

// ---------------------------------------------------------------------------
// Network
// ---------------------------------------------------------------------------

Result<std::vector<ProcNetConn>, std::string> ProcInspector::net(int pid) const {
    std::vector<ProcNetConn> connections;

    // Build set of inodes owned by this process
    std::set<std::string> proc_inodes;
    std::string fd_dir = "/proc/" + std::to_string(pid) + "/fd";

    try {
        for (const auto& entry : fs::directory_iterator(fd_dir)) {
            auto link = fs::read_symlink(entry.path());
            std::string link_str = link.string();
            if (link_str.rfind("socket:[", 0) == 0) {
                proc_inodes.insert(link_str.substr(8, link_str.size() - 9));
            }
        }
    } catch (...) {}

    if (proc_inodes.empty()) {
        return Result<std::vector<ProcNetConn>, std::string>::ok(connections);
    }

    // Scan /proc/net/tcp, tcp6, udp, udp6
    for (const auto& proto : {"tcp", "tcp6", "udp", "udp6"}) {
        std::ifstream f(std::string("/proc/net/") + proto);
        if (!f.is_open()) continue;

        std::string line;
        std::getline(f, line); // header

        while (std::getline(f, line)) {
            std::istringstream ss(line);
            std::string idx, local, remote, st, txrx, tr, retrnsmt, uid_str, timeout, inode;
            ss >> idx >> local >> remote >> st >> txrx >> tr >> retrnsmt >> uid_str >> timeout >> inode;

            if (proc_inodes.count(inode) == 0) continue;

            ProcNetConn conn;
            conn.protocol = proto;

            auto colon = local.rfind(':');
            if (colon != std::string::npos) {
                conn.local_addr = hex_to_ip(local.substr(0, colon));
                conn.local_port = hex_to_port(local.substr(colon + 1));
            }

            colon = remote.rfind(':');
            if (colon != std::string::npos) {
                conn.remote_addr = hex_to_ip(remote.substr(0, colon));
                conn.remote_port = hex_to_port(remote.substr(colon + 1));
            }

            int state_int = std::stoi(st, nullptr, 16);
            switch (state_int) {
                case 1: conn.state = "ESTABLISHED"; break;
                case 2: conn.state = "SYN_SENT"; break;
                case 6: conn.state = "TIME_WAIT"; break;
                case 10: conn.state = "LISTEN"; break;
                default: conn.state = st; break;
            }

            connections.push_back(conn);
        }
    }

    return Result<std::vector<ProcNetConn>, std::string>::ok(connections);
}

// ---------------------------------------------------------------------------
// Environment
// ---------------------------------------------------------------------------

Result<std::map<std::string, std::string>, std::string> ProcInspector::env(int pid) const {
    auto environ = read_proc_file(pid, "environ");
    if (!environ.has_value()) {
        return Result<std::map<std::string, std::string>, std::string>::error(environ.error());
    }

    std::map<std::string, std::string> env_map;
    std::string content = environ.value();

    size_t pos = 0;
    while (pos < content.size()) {
        auto null_pos = content.find('\0', pos);
        if (null_pos == std::string::npos) null_pos = content.size();

        std::string entry = content.substr(pos, null_pos - pos);
        auto eq = entry.find('=');
        if (eq != std::string::npos) {
            env_map[entry.substr(0, eq)] = entry.substr(eq + 1);
        }

        pos = null_pos + 1;
    }

    return Result<std::map<std::string, std::string>, std::string>::ok(env_map);
}

// ---------------------------------------------------------------------------
// Signal
// ---------------------------------------------------------------------------

Result<void, std::string> ProcInspector::signal(int pid, int sig) const {
    if (kill(pid, sig) != 0) {
        return Result<void, std::string>::error(
            "kill(" + std::to_string(pid) + ", " + std::to_string(sig) + ") failed: " +
            strerror(errno));
    }
    return Result<void, std::string>::ok();
}

// ---------------------------------------------------------------------------
// Renice
// ---------------------------------------------------------------------------

Result<void, std::string> ProcInspector::renice(int pid, int nice_level) const {
    auto res = run_cmd("renice " + std::to_string(nice_level) + " -p " +
                        std::to_string(pid) + " 2>&1");
    if (!res.has_value()) {
        return Result<void, std::string>::error("renice failed: " + res.error());
    }
    return Result<void, std::string>::ok();
}

// ---------------------------------------------------------------------------
// Find
// ---------------------------------------------------------------------------

Result<std::vector<ProcInfo>, std::string> ProcInspector::find(const std::string& name) const {
    std::vector<ProcInfo> results;
    std::string lower_name = name;
    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);

    for (const auto& entry : fs::directory_iterator("/proc")) {
        std::string fname = entry.path().filename().string();
        int pid = 0;
        try { pid = std::stoi(fname); } catch (...) { continue; }

        auto info_res = info(pid);
        if (!info_res.has_value()) continue;

        std::string lower_proc = info_res.value().name;
        std::transform(lower_proc.begin(), lower_proc.end(), lower_proc.begin(), ::tolower);

        std::string lower_cmd = info_res.value().cmdline;
        std::transform(lower_cmd.begin(), lower_cmd.end(), lower_cmd.begin(), ::tolower);

        if (lower_proc.find(lower_name) != std::string::npos ||
            lower_cmd.find(lower_name) != std::string::npos) {
            results.push_back(info_res.value());
        }
    }

    return Result<std::vector<ProcInfo>, std::string>::ok(results);
}

// ---------------------------------------------------------------------------
// IO
// ---------------------------------------------------------------------------

Result<ProcIO, std::string> ProcInspector::io(int pid) const {
    auto content = read_proc_file(pid, "io");
    if (!content.has_value()) {
        return Result<ProcIO, std::string>::error(content.error());
    }

    ProcIO io;
    std::istringstream stream(content.value());
    std::string line;

    while (std::getline(stream, line)) {
        if (line.rfind("rchar:", 0) == 0) {} // skip
        else if (line.rfind("wchar:", 0) == 0) {} // skip
        else if (line.rfind("syscr:", 0) == 0) {
            try { io.read_syscalls = std::stoull(line.substr(6)); } catch (...) {}
        } else if (line.rfind("syscw:", 0) == 0) {
            try { io.write_syscalls = std::stoull(line.substr(6)); } catch (...) {}
        } else if (line.rfind("read_bytes:", 0) == 0) {
            try { io.read_bytes = std::stoull(line.substr(12)); } catch (...) {}
        } else if (line.rfind("write_bytes:", 0) == 0) {
            try { io.write_bytes = std::stoull(line.substr(13)); } catch (...) {}
        } else if (line.rfind("cancelled_write_bytes:", 0) == 0) {
            try { io.cancelled_write_bytes = std::stoull(line.substr(23)); } catch (...) {}
        }
    }

    return Result<ProcIO, std::string>::ok(io);
}

// ---------------------------------------------------------------------------
// Limits
// ---------------------------------------------------------------------------

Result<std::vector<ProcLimit>, std::string> ProcInspector::limits(int pid) const {
    auto content = read_proc_file(pid, "limits");
    if (!content.has_value()) {
        return Result<std::vector<ProcLimit>, std::string>::error(content.error());
    }

    std::vector<ProcLimit> lims;
    std::istringstream stream(content.value());
    std::string line;

    std::getline(stream, line); // Skip header

    while (std::getline(stream, line)) {
        if (line.empty()) continue;

        // Format: "Limit Name                     Soft Limit           Hard Limit           Units"
        // The limit name can have spaces, so we parse from the right
        ProcLimit lim;

        // Find units (last column)
        auto last_space = line.rfind("  ");
        if (last_space == std::string::npos) continue;

        // Parse backwards
        std::regex limit_re(R"(^(.+?)\s{2,}(\S+)\s{2,}(\S+)\s{2,}(\S+)\s*$)");
        std::smatch m;
        if (std::regex_search(line, m, limit_re)) {
            lim.name = m[1].str();
            lim.soft = m[2].str();
            lim.hard = m[3].str();
            lim.units = m[4].str();
            lims.push_back(lim);
        }
    }

    return Result<std::vector<ProcLimit>, std::string>::ok(lims);
}

} // namespace straylight
