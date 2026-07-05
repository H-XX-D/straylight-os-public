/**
 * StrayLight Mirror State Capture — Implementation.
 */

#include "state_capture.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace straylight::mirror {

// ---------------------------------------------------------------------------
// Serialization helpers
// ---------------------------------------------------------------------------

static void write_u32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

static void write_u64(std::vector<uint8_t>& buf, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        buf.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
    }
}

static void write_string(std::vector<uint8_t>& buf, const std::string& s) {
    write_u32(buf, static_cast<uint32_t>(s.size()));
    buf.insert(buf.end(), s.begin(), s.end());
}

static uint32_t read_u32(const uint8_t*& ptr) {
    uint32_t v = ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24);
    ptr += 4;
    return v;
}

static uint64_t read_u64(const uint8_t*& ptr) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<uint64_t>(ptr[i]) << (i * 8);
    }
    ptr += 8;
    return v;
}

static std::string read_string(const uint8_t*& ptr) {
    uint32_t len = read_u32(ptr);
    std::string s(reinterpret_cast<const char*>(ptr), len);
    ptr += len;
    return s;
}

// ---------------------------------------------------------------------------
// SystemState serialization
// ---------------------------------------------------------------------------

std::vector<uint8_t> SystemState::serialize() const {
    std::vector<uint8_t> buf;

    // Magic header.
    const char magic[] = "SLMS";  // StrayLight Mirror State
    buf.insert(buf.end(), magic, magic + 4);

    // Version.
    write_u32(buf, 1);

    // Timestamp + hostname + kernel.
    write_u64(buf, capture_timestamp_ms);
    write_string(buf, hostname);
    write_string(buf, kernel_version);

    // Processes.
    write_u32(buf, static_cast<uint32_t>(processes.size()));
    for (const auto& p : processes) {
        write_u32(buf, static_cast<uint32_t>(p.pid));
        write_u32(buf, static_cast<uint32_t>(p.ppid));
        write_string(buf, p.name);
        write_string(buf, p.cmdline);
        write_string(buf, p.state);
        write_u64(buf, p.rss_kb);
        write_u32(buf, static_cast<uint32_t>(p.open_files.size()));
        for (const auto& f : p.open_files) write_string(buf, f);
        write_u32(buf, static_cast<uint32_t>(p.network_connections.size()));
        for (const auto& c : p.network_connections) write_string(buf, c);
    }

    // Daemons.
    write_u32(buf, static_cast<uint32_t>(daemons.size()));
    for (const auto& d : daemons) {
        write_string(buf, d.name);
        write_u32(buf, static_cast<uint32_t>(d.pid));
        write_string(buf, d.config_json);
        write_string(buf, d.runtime_json);
        write_u32(buf, d.running ? 1 : 0);
    }

    // VPU state.
    write_u64(buf, vpu_state.total_vram_bytes);
    write_u64(buf, vpu_state.used_vram_bytes);
    write_u32(buf, static_cast<uint32_t>(vpu_state.slabs.size()));
    for (const auto& s : vpu_state.slabs) {
        write_u32(buf, s.slab_id);
        write_u64(buf, s.base_offset);
        write_u64(buf, s.size_bytes);
        write_u32(buf, s.allocated ? 1 : 0);
        write_string(buf, s.owner);
        write_u32(buf, s.ref_count);
    }
    write_string(buf, vpu_state.slab_bitmap);

    // Network state.
    write_u32(buf, static_cast<uint32_t>(network_state.interfaces.size()));
    for (const auto& iface : network_state.interfaces) {
        write_string(buf, iface.name);
        write_string(buf, iface.ipv4_addr);
        write_string(buf, iface.ipv6_addr);
        write_string(buf, iface.mac_addr);
        write_u32(buf, iface.up ? 1 : 0);
        write_u32(buf, static_cast<uint32_t>(iface.mtu));
    }
    write_u32(buf, static_cast<uint32_t>(network_state.routes.size()));
    for (const auto& r : network_state.routes) {
        write_string(buf, r.destination);
        write_string(buf, r.gateway);
        write_string(buf, r.interface);
        write_u32(buf, static_cast<uint32_t>(r.metric));
    }
    write_u32(buf, static_cast<uint32_t>(network_state.iptables_rules.size()));
    for (const auto& r : network_state.iptables_rules) {
        write_string(buf, r.chain);
        write_string(buf, r.rule);
    }
    write_u32(buf, static_cast<uint32_t>(network_state.mesh_nodes.size()));
    for (const auto& m : network_state.mesh_nodes) {
        write_string(buf, m.node_id);
        write_string(buf, m.address);
        write_u32(buf, m.reachable ? 1 : 0);
        write_u32(buf, static_cast<uint32_t>(m.latency_ms));
    }

    return buf;
}

Result<SystemState, std::string> SystemState::deserialize(const std::vector<uint8_t>& data) {
    if (data.size() < 12) {
        return Result<SystemState, std::string>::error("data too short");
    }

    const uint8_t* ptr = data.data();

    // Check magic.
    if (memcmp(ptr, "SLMS", 4) != 0) {
        return Result<SystemState, std::string>::error("invalid magic header");
    }
    ptr += 4;

    uint32_t version = read_u32(ptr);
    if (version != 1) {
        return Result<SystemState, std::string>::error("unsupported version: " + std::to_string(version));
    }

    SystemState state;
    state.capture_timestamp_ms = read_u64(ptr);
    state.hostname = read_string(ptr);
    state.kernel_version = read_string(ptr);

    // Processes.
    uint32_t num_procs = read_u32(ptr);
    state.processes.resize(num_procs);
    for (uint32_t i = 0; i < num_procs; ++i) {
        auto& p = state.processes[i];
        p.pid = static_cast<int>(read_u32(ptr));
        p.ppid = static_cast<int>(read_u32(ptr));
        p.name = read_string(ptr);
        p.cmdline = read_string(ptr);
        p.state = read_string(ptr);
        p.rss_kb = read_u64(ptr);
        uint32_t nf = read_u32(ptr);
        p.open_files.resize(nf);
        for (uint32_t j = 0; j < nf; ++j) p.open_files[j] = read_string(ptr);
        uint32_t nc = read_u32(ptr);
        p.network_connections.resize(nc);
        for (uint32_t j = 0; j < nc; ++j) p.network_connections[j] = read_string(ptr);
    }

    // Daemons.
    uint32_t num_daemons = read_u32(ptr);
    state.daemons.resize(num_daemons);
    for (uint32_t i = 0; i < num_daemons; ++i) {
        auto& d = state.daemons[i];
        d.name = read_string(ptr);
        d.pid = static_cast<int>(read_u32(ptr));
        d.config_json = read_string(ptr);
        d.runtime_json = read_string(ptr);
        d.running = read_u32(ptr) != 0;
    }

    // VPU state.
    state.vpu_state.total_vram_bytes = read_u64(ptr);
    state.vpu_state.used_vram_bytes = read_u64(ptr);
    uint32_t num_slabs = read_u32(ptr);
    state.vpu_state.slabs.resize(num_slabs);
    for (uint32_t i = 0; i < num_slabs; ++i) {
        auto& s = state.vpu_state.slabs[i];
        s.slab_id = read_u32(ptr);
        s.base_offset = read_u64(ptr);
        s.size_bytes = read_u64(ptr);
        s.allocated = read_u32(ptr) != 0;
        s.owner = read_string(ptr);
        s.ref_count = read_u32(ptr);
    }
    state.vpu_state.slab_bitmap = read_string(ptr);

    // Network state.
    uint32_t ni = read_u32(ptr);
    state.network_state.interfaces.resize(ni);
    for (uint32_t i = 0; i < ni; ++i) {
        auto& iface = state.network_state.interfaces[i];
        iface.name = read_string(ptr);
        iface.ipv4_addr = read_string(ptr);
        iface.ipv6_addr = read_string(ptr);
        iface.mac_addr = read_string(ptr);
        iface.up = read_u32(ptr) != 0;
        iface.mtu = static_cast<int>(read_u32(ptr));
    }
    uint32_t nr = read_u32(ptr);
    state.network_state.routes.resize(nr);
    for (uint32_t i = 0; i < nr; ++i) {
        auto& r = state.network_state.routes[i];
        r.destination = read_string(ptr);
        r.gateway = read_string(ptr);
        r.interface = read_string(ptr);
        r.metric = static_cast<int>(read_u32(ptr));
    }
    uint32_t nip = read_u32(ptr);
    state.network_state.iptables_rules.resize(nip);
    for (uint32_t i = 0; i < nip; ++i) {
        auto& r = state.network_state.iptables_rules[i];
        r.chain = read_string(ptr);
        r.rule = read_string(ptr);
    }
    uint32_t nm = read_u32(ptr);
    state.network_state.mesh_nodes.resize(nm);
    for (uint32_t i = 0; i < nm; ++i) {
        auto& m = state.network_state.mesh_nodes[i];
        m.node_id = read_string(ptr);
        m.address = read_string(ptr);
        m.reachable = read_u32(ptr) != 0;
        m.latency_ms = static_cast<int>(read_u32(ptr));
    }

    return Result<SystemState, std::string>::ok(std::move(state));
}

// ---------------------------------------------------------------------------
// Process list capture
// ---------------------------------------------------------------------------

static std::string read_file_contents(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::ostringstream oss;
    oss << f.rdbuf();
    return oss.str();
}

static std::string read_proc_line(const std::string& path) {
    std::ifstream f(path);
    std::string line;
    if (f) std::getline(f, line);
    return line;
}

Result<std::vector<ProcessInfo>, std::string> StateCapture::capture_process_list() {
    std::vector<ProcessInfo> procs;
    namespace fs = std::filesystem;

    if (!fs::exists("/proc")) {
        return Result<std::vector<ProcessInfo>, std::string>::error(
            "/proc not available");
    }

    std::error_code ec;
    for (auto& entry : fs::directory_iterator("/proc", ec)) {
        std::string name = entry.path().filename().string();
        if (name.empty() || !std::isdigit(name[0])) continue;

        int pid = std::stoi(name);
        std::string proc_dir = "/proc/" + name;

        ProcessInfo info;
        info.pid = pid;

        // Read status.
        std::ifstream status_f(proc_dir + "/status");
        if (status_f) {
            std::string line;
            while (std::getline(status_f, line)) {
                if (line.rfind("Name:", 0) == 0) {
                    info.name = line.substr(6);
                    while (!info.name.empty() && info.name[0] == '\t')
                        info.name = info.name.substr(1);
                } else if (line.rfind("PPid:", 0) == 0) {
                    info.ppid = std::stoi(line.substr(5));
                } else if (line.rfind("State:", 0) == 0) {
                    info.state = line.substr(7, 1);
                } else if (line.rfind("VmRSS:", 0) == 0) {
                    std::string rss_str = line.substr(6);
                    // Remove "kB" suffix and whitespace.
                    auto pos = rss_str.find_first_of("0123456789");
                    if (pos != std::string::npos) {
                        info.rss_kb = std::stoull(rss_str.substr(pos));
                    }
                }
            }
        }

        // Read cmdline.
        std::string cmdline = read_file_contents(proc_dir + "/cmdline");
        std::replace(cmdline.begin(), cmdline.end(), '\0', ' ');
        if (!cmdline.empty() && cmdline.back() == ' ') cmdline.pop_back();
        info.cmdline = cmdline;

        // Read open file descriptors.
        std::string fd_dir = proc_dir + "/fd";
        if (fs::exists(fd_dir)) {
            std::error_code ec2;
            for (auto& fd_entry : fs::directory_iterator(fd_dir, ec2)) {
                char link_target[1024];
                ssize_t len = readlink(fd_entry.path().c_str(), link_target, sizeof(link_target) - 1);
                if (len > 0) {
                    link_target[len] = '\0';
                    std::string target(link_target);
                    // Skip /dev/null, pipes, anon_inode, etc. for cleanliness.
                    if (target.rfind("/dev/null", 0) != 0 &&
                        target.rfind("pipe:", 0) != 0 &&
                        target.rfind("anon_inode:", 0) != 0) {
                        info.open_files.push_back(target);
                    }
                }
            }
        }

        // Read network connections from /proc/PID/net/tcp + tcp6.
        for (const auto& proto : {"tcp", "tcp6", "udp", "udp6"}) {
            std::ifstream nf(proc_dir + "/net/" + proto);
            if (!nf) continue;
            std::string hdr;
            std::getline(nf, hdr); // Skip header.
            std::string line;
            while (std::getline(nf, line)) {
                if (!line.empty()) {
                    info.network_connections.push_back(std::string(proto) + ": " + line);
                }
            }
        }

        procs.push_back(std::move(info));
    }

    return Result<std::vector<ProcessInfo>, std::string>::ok(std::move(procs));
}

// ---------------------------------------------------------------------------
// Service state capture
// ---------------------------------------------------------------------------

Result<std::vector<DaemonSnapshot>, std::string> StateCapture::capture_service_state() {
    std::vector<DaemonSnapshot> daemons;
    namespace fs = std::filesystem;

    const std::string run_dir = "/var/run/straylight";
    if (!fs::exists(run_dir)) {
        return Result<std::vector<DaemonSnapshot>, std::string>::ok(std::move(daemons));
    }

    // Find all PID files — each represents a running daemon.
    std::error_code ec;
    for (auto& entry : fs::directory_iterator(run_dir, ec)) {
        std::string name = entry.path().filename().string();
        if (name.size() < 5 || name.substr(name.size() - 4) != ".pid") continue;

        DaemonSnapshot snap;
        snap.name = name.substr(0, name.size() - 4);

        // Read PID.
        std::ifstream pidf(entry.path());
        if (pidf) pidf >> snap.pid;

        // Check if actually running.
        snap.running = (kill(snap.pid, 0) == 0);

        // Read daemon config if available.
        std::string config_path = "/etc/straylight/" + snap.name + ".conf";
        if (fs::exists(config_path)) {
            snap.config_json = read_file_contents(config_path);
        }

        // Read runtime state if the daemon exports it.
        std::string state_path = run_dir + "/" + snap.name + ".state";
        if (fs::exists(state_path)) {
            snap.runtime_json = read_file_contents(state_path);
        }

        daemons.push_back(std::move(snap));
    }

    return Result<std::vector<DaemonSnapshot>, std::string>::ok(std::move(daemons));
}

// ---------------------------------------------------------------------------
// VPU state capture
// ---------------------------------------------------------------------------

Result<VpuState, std::string> StateCapture::capture_vpu_state() {
    VpuState state;
    namespace fs = std::filesystem;

    const std::string vpu_base = "/sys/kernel/straylight-vpu";

    if (!fs::exists(vpu_base)) {
        // VPU not present — return empty state.
        state.total_vram_bytes = 0;
        state.used_vram_bytes = 0;
        return Result<VpuState, std::string>::ok(std::move(state));
    }

    // Read total VRAM.
    std::ifstream total_f(vpu_base + "/total_vram");
    if (total_f) total_f >> state.total_vram_bytes;

    // Read used VRAM.
    std::ifstream used_f(vpu_base + "/used_vram");
    if (used_f) used_f >> state.used_vram_bytes;

    // Read slab bitmap.
    std::ifstream bitmap_f(vpu_base + "/slab_bitmap");
    if (bitmap_f) std::getline(bitmap_f, state.slab_bitmap);

    // Read individual slab info.
    std::string slabs_dir = vpu_base + "/slabs";
    if (fs::exists(slabs_dir)) {
        std::error_code ec;
        for (auto& entry : fs::directory_iterator(slabs_dir, ec)) {
            VpuSlabInfo slab;

            std::string slab_dir = entry.path().string();
            std::string id_str = entry.path().filename().string();

            // Parse slab ID from directory name (e.g., "slab_0").
            auto underscore = id_str.find('_');
            if (underscore != std::string::npos) {
                slab.slab_id = static_cast<uint32_t>(std::stoul(id_str.substr(underscore + 1)));
            }

            std::ifstream base_f(slab_dir + "/base_offset");
            if (base_f) base_f >> slab.base_offset;

            std::ifstream size_f(slab_dir + "/size");
            if (size_f) size_f >> slab.size_bytes;

            std::ifstream alloc_f(slab_dir + "/allocated");
            if (alloc_f) {
                int v = 0;
                alloc_f >> v;
                slab.allocated = (v != 0);
            }

            std::ifstream owner_f(slab_dir + "/owner");
            if (owner_f) std::getline(owner_f, slab.owner);

            std::ifstream ref_f(slab_dir + "/ref_count");
            if (ref_f) ref_f >> slab.ref_count;

            state.slabs.push_back(std::move(slab));
        }
    }

    return Result<VpuState, std::string>::ok(std::move(state));
}

// ---------------------------------------------------------------------------
// Network state capture
// ---------------------------------------------------------------------------

static std::string exec_command(const std::string& cmd) {
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) {
        result += buf;
    }
    pclose(pipe);
    return result;
}

Result<NetworkState, std::string> StateCapture::capture_network_state() {
    NetworkState net;

    // Capture interfaces from /sys/class/net.
    namespace fs = std::filesystem;
    if (fs::exists("/sys/class/net")) {
        std::error_code ec;
        for (auto& entry : fs::directory_iterator("/sys/class/net", ec)) {
            NetworkState::Interface iface;
            iface.name = entry.path().filename().string();

            // Read MAC.
            std::ifstream mac_f(entry.path().string() + "/address");
            if (mac_f) std::getline(mac_f, iface.mac_addr);

            // Read MTU.
            std::ifstream mtu_f(entry.path().string() + "/mtu");
            if (mtu_f) mtu_f >> iface.mtu;

            // Read operstate.
            std::ifstream state_f(entry.path().string() + "/operstate");
            std::string state;
            if (state_f) std::getline(state_f, state);
            iface.up = (state == "up");

            // Get IP from ip addr show.
            std::string ip_out = exec_command("ip -4 addr show " + iface.name + " 2>/dev/null");
            auto inet_pos = ip_out.find("inet ");
            if (inet_pos != std::string::npos) {
                auto start = inet_pos + 5;
                auto end = ip_out.find_first_of(" /", start);
                if (end != std::string::npos) {
                    iface.ipv4_addr = ip_out.substr(start, end - start);
                }
            }

            std::string ip6_out = exec_command("ip -6 addr show " + iface.name + " scope global 2>/dev/null");
            auto inet6_pos = ip6_out.find("inet6 ");
            if (inet6_pos != std::string::npos) {
                auto start = inet6_pos + 6;
                auto end = ip6_out.find_first_of(" /", start);
                if (end != std::string::npos) {
                    iface.ipv6_addr = ip6_out.substr(start, end - start);
                }
            }

            net.interfaces.push_back(std::move(iface));
        }
    }

    // Capture routes.
    std::string route_out = exec_command("ip route show 2>/dev/null");
    std::istringstream route_stream(route_out);
    std::string line;
    while (std::getline(route_stream, line)) {
        if (line.empty()) continue;
        NetworkState::Route route;

        // Parse "default via 192.0.2.1 dev eth0 metric 100" or "192.0.2.0/24 dev eth0 ..."
        std::istringstream ls(line);
        ls >> route.destination;

        std::string token;
        while (ls >> token) {
            if (token == "via") {
                ls >> route.gateway;
            } else if (token == "dev") {
                ls >> route.interface;
            } else if (token == "metric") {
                ls >> route.metric;
            }
        }

        net.routes.push_back(std::move(route));
    }

    // Capture iptables rules.
    for (const auto& chain : {"INPUT", "OUTPUT", "FORWARD"}) {
        std::string ipt_out = exec_command(
            std::string("iptables -S ") + chain + " 2>/dev/null");
        std::istringstream ipt_stream(ipt_out);
        std::string rule_line;
        while (std::getline(ipt_stream, rule_line)) {
            if (rule_line.empty()) continue;
            NetworkState::IptablesRule rule;
            rule.chain = chain;
            rule.rule = rule_line;
            net.iptables_rules.push_back(std::move(rule));
        }
    }

    // Capture mesh nodes from StrayLight mesh daemon.
    std::string mesh_state = "/run/straylight/mesh/nodes";
    if (fs::exists(mesh_state)) {
        std::ifstream mf(mesh_state);
        std::string mline;
        while (std::getline(mf, mline)) {
            if (mline.empty()) continue;
            // Format: node_id address reachable latency_ms
            NetworkState::MeshNode node;
            std::istringstream ms(mline);
            std::string reachable_str;
            ms >> node.node_id >> node.address >> reachable_str >> node.latency_ms;
            node.reachable = (reachable_str == "1" || reachable_str == "true");
            net.mesh_nodes.push_back(std::move(node));
        }
    }

    return Result<NetworkState, std::string>::ok(std::move(net));
}

// ---------------------------------------------------------------------------
// Full capture
// ---------------------------------------------------------------------------

Result<SystemState, std::string> StateCapture::capture_all() {
    SystemState state;

    auto now = std::chrono::system_clock::now();
    state.capture_timestamp_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count());

    // Hostname.
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        state.hostname = hostname;
    }

    // Kernel version.
    std::ifstream version_f("/proc/version");
    if (version_f) {
        std::getline(version_f, state.kernel_version);
    }

    // Capture subsystems.
    auto procs = capture_process_list();
    if (procs) state.processes = std::move(procs.value());

    auto daemons = capture_service_state();
    if (daemons) state.daemons = std::move(daemons.value());

    auto vpu = capture_vpu_state();
    if (vpu) state.vpu_state = std::move(vpu.value());

    auto net = capture_network_state();
    if (net) state.network_state = std::move(net.value());

    return Result<SystemState, std::string>::ok(std::move(state));
}

} // namespace straylight::mirror
