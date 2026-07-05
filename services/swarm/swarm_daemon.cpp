// services/swarm/swarm_daemon.cpp
#include "swarm_daemon.h"
#include <straylight/log.h>

#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>
#include <thread>
#include <chrono>

#ifdef __linux__
#include <unistd.h>
#elif defined(__APPLE__)
#include <unistd.h>
#endif

namespace straylight {

// ---------------------------------------------------------------------------
// UUID generation
// ---------------------------------------------------------------------------

std::string SwarmDaemon::generate_uuid() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);

    auto r = [&]() { return dist(gen); };

    // UUID v4 format: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
    uint32_t a = r(), b = r(), c = r(), d = r();
    // Set version 4 and variant bits
    b = (b & 0xFFFF0FFF) | 0x00004000;  // version 4
    c = (c & 0x3FFFFFFF) | 0x80000000;  // variant 1

    char buf[37];
    snprintf(buf, sizeof(buf),
             "%08x-%04x-%04x-%04x-%04x%08x",
             a,
             (b >> 16) & 0xFFFF,
             b & 0xFFFF,
             (c >> 16) & 0xFFFF,
             c & 0xFFFF,
             d);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Build self node
// ---------------------------------------------------------------------------

SwarmNode SwarmDaemon::build_self_node(const Config& cfg) {
    SwarmNode self;
    self.node_id = generate_uuid();
    self.port = static_cast<uint16_t>(cfg.get<int>("port", 7700));

    // Hostname
    char hostname_buf[256];
    if (gethostname(hostname_buf, sizeof(hostname_buf)) == 0) {
        self.hostname = hostname_buf;
    } else {
        self.hostname = "unknown";
    }

    // IP address — try to detect the primary interface IP.
    // On Linux, read the default route interface and its address.
    // On macOS, use a simpler approach.
    self.ip_address = cfg.get<std::string>("bind_address", "0.0.0.0");
    if (self.ip_address == "0.0.0.0") {
        // Try to auto-detect via a UDP connect trick
        int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (sock >= 0) {
            struct sockaddr_in remote{};
            remote.sin_family = AF_INET;
            remote.sin_port = htons(53);
            inet_pton(AF_INET, "8.8.8.8", &remote.sin_addr);

            if (::connect(sock, reinterpret_cast<struct sockaddr*>(&remote), sizeof(remote)) == 0) {
                struct sockaddr_in local{};
                socklen_t len = sizeof(local);
                if (getsockname(sock, reinterpret_cast<struct sockaddr*>(&local), &len) == 0) {
                    char ip_str[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &local.sin_addr, ip_str, sizeof(ip_str));
                    self.ip_address = ip_str;
                }
            }
            ::close(sock);
        }
    }

    // CPU info
    std::ifstream cpuinfo("/proc/cpuinfo");
    if (cpuinfo.is_open()) {
        std::string line;
        int count = 0;
        while (std::getline(cpuinfo, line)) {
            if (line.rfind("processor", 0) == 0) count++;
        }
        self.cpu_cores = count;
    }
    if (self.cpu_cores == 0) {
        // Fallback: sysconf
        long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
        self.cpu_cores = (nprocs > 0) ? static_cast<int>(nprocs) : 1;
    }

    // Memory
    std::ifstream meminfo("/proc/meminfo");
    if (meminfo.is_open()) {
        std::string line;
        while (std::getline(meminfo, line)) {
            if (line.rfind("MemTotal:", 0) == 0) {
                std::istringstream iss(line.substr(9));
                uint64_t kb = 0;
                iss >> kb;
                self.mem_total = kb * 1024ULL;
            } else if (line.rfind("MemAvailable:", 0) == 0) {
                std::istringstream iss(line.substr(13));
                uint64_t kb = 0;
                iss >> kb;
                self.mem_free = kb * 1024ULL;
            }
        }
    }
    if (self.mem_total == 0) {
        // macOS fallback
        long pages = sysconf(_SC_PHYS_PAGES);
        long page_size = sysconf(_SC_PAGE_SIZE);
        if (pages > 0 && page_size > 0) {
            self.mem_total = static_cast<uint64_t>(pages) * static_cast<uint64_t>(page_size);
            self.mem_free = self.mem_total / 2;  // rough estimate
        }
    }

    // GPU detection (quick check via nvidia-smi)
    FILE* pipe = popen("nvidia-smi --query-gpu=count --format=csv,noheader 2>/dev/null", "r");
    if (pipe) {
        char buf[64];
        if (fgets(buf, sizeof(buf), pipe)) {
            try { self.gpu_count = std::stoi(buf); } catch (...) {}
        }
        pclose(pipe);
    }

    if (self.gpu_count > 0) {
        FILE* vram_pipe = popen("nvidia-smi --query-gpu=memory.total,memory.free "
                                "--format=csv,noheader,nounits 2>/dev/null", "r");
        if (vram_pipe) {
            char buf[128];
            uint64_t total_vram = 0, free_vram = 0;
            while (fgets(buf, sizeof(buf), vram_pipe)) {
                std::istringstream iss(buf);
                std::string tot_str, free_str;
                if (std::getline(iss, tot_str, ',') && std::getline(iss, free_str)) {
                    auto trim = [](std::string& s) {
                        while (!s.empty() && (s.front() == ' ')) s.erase(s.begin());
                        while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
                    };
                    trim(tot_str); trim(free_str);
                    try {
                        total_vram += static_cast<uint64_t>(std::stoi(tot_str)) * 1024ULL * 1024ULL;
                        free_vram += static_cast<uint64_t>(std::stoi(free_str)) * 1024ULL * 1024ULL;
                    } catch (...) {}
                }
            }
            pclose(vram_pipe);
            self.vram_total = total_vram;
            self.vram_free = free_vram;
        }
    }

    // Load average
    std::ifstream loadavg("/proc/loadavg");
    if (loadavg.is_open()) {
        loadavg >> self.load_1m;
    }

    self.is_self = true;
    return self;
}

// ---------------------------------------------------------------------------
// DaemonBase overrides
// ---------------------------------------------------------------------------

Result<void, SLError> SwarmDaemon::init(const Config& cfg) {
    heartbeat_interval_s_ = cfg.get<int>("heartbeat_interval_seconds", 10);
    stale_timeout_s_      = cfg.get<int>("stale_timeout_seconds", 60);
    task_poll_interval_s_ = cfg.get<int>("task_poll_interval_seconds", 2);

    // Build self node
    SwarmNode self = build_self_node(cfg);

    SL_INFO("swarm: local node: {} ({}) — {} cores, {} MiB RAM, {} GPU(s)",
            self.hostname, self.ip_address, self.cpu_cores,
            self.mem_total / (1024 * 1024), self.gpu_count);

    // Start discovery
    auto dr = discovery_.start(self);
    if (!dr.has_value()) {
        SL_WARN("swarm: discovery start had issues: {}", dr.error());
        // Non-fatal — we can still work as a single node
    }

    // Initialize node client
    client_.init(discovery_);

    // Create orchestrator
    orchestrator_ = std::make_unique<SwarmOrchestrator>(discovery_, client_);

    auto now = std::chrono::steady_clock::now();
    last_heartbeat_ = now;
    last_task_poll_ = now;
    last_eviction_ = now;

    SL_INFO("swarm: initialized (heartbeat={}s, stale_timeout={}s)",
            heartbeat_interval_s_, stale_timeout_s_);

    return Result<void, SLError>::ok();
}

Result<void, SLError> SwarmDaemon::tick() {
    auto now = std::chrono::steady_clock::now();

    // Heartbeat
    auto since_hb = std::chrono::duration_cast<std::chrono::seconds>(now - last_heartbeat_).count();
    if (since_hb >= heartbeat_interval_s_) {
        discovery_.send_heartbeat();
        last_heartbeat_ = now;
    }

    // Receive incoming heartbeats (non-blocking)
    discovery_.receive_heartbeats();

    // Evict stale nodes periodically
    auto since_evict = std::chrono::duration_cast<std::chrono::seconds>(now - last_eviction_).count();
    if (since_evict >= stale_timeout_s_ / 2) {
        discovery_.evict_stale_nodes(std::chrono::seconds(stale_timeout_s_));
        last_eviction_ = now;
    }

    // Process tasks
    auto since_poll = std::chrono::duration_cast<std::chrono::seconds>(now - last_task_poll_).count();
    if (since_poll >= task_poll_interval_s_) {
        orchestrator_->process_tasks();
        last_task_poll_ = now;
    }

    // Sleep to avoid busy-spin (2s to stay responsive)
    std::this_thread::sleep_for(std::chrono::seconds(2));
    return Result<void, SLError>::ok();
}

void SwarmDaemon::shutdown() {
    SL_INFO("swarm: shutting down");
    discovery_.stop();
}

// ---------------------------------------------------------------------------
// D-Bus method handlers
// ---------------------------------------------------------------------------

std::vector<SwarmNode> SwarmDaemon::dbus_list_nodes() const {
    return discovery_.nodes();
}

std::string SwarmDaemon::dbus_submit_task(const std::string& command, const std::string& strategy) {
    std::lock_guard lock(mutex_);

    SwarmTask task;
    task.command = command;

    if (strategy == "gpu_affinity" || strategy == "gpu") {
        task.strategy = PlacementStrategy::GpuAffinity;
    } else if (strategy == "cpu_affinity" || strategy == "cpu") {
        task.strategy = PlacementStrategy::CpuAffinity;
    } else if (strategy == "spread") {
        task.strategy = PlacementStrategy::Spread;
    } else if (strategy == "pack") {
        task.strategy = PlacementStrategy::Pack;
    } else {
        task.strategy = PlacementStrategy::GpuAffinity;
    }

    auto r = orchestrator_->submit_task(task);
    if (!r.has_value()) {
        SL_ERROR("swarm: submit failed: {}", r.error());
        return "";
    }
    return r.value();
}

std::string SwarmDaemon::dbus_task_status(const std::string& task_id) const {
    auto r = orchestrator_->task_status(task_id);
    if (!r.has_value()) return "unknown";

    auto& s = r.value();
    switch (s.state) {
        case TaskState::Pending:   return "pending";
        case TaskState::Running:   return "running";
        case TaskState::Completed: return "completed";
        case TaskState::Failed:    return "failed";
        case TaskState::Cancelled: return "cancelled";
    }
    return "unknown";
}

void SwarmDaemon::dbus_cancel_task(const std::string& task_id) {
    std::lock_guard lock(mutex_);
    auto r = orchestrator_->cancel_task(task_id);
    if (!r.has_value()) {
        SL_WARN("swarm: cancel failed: {}", r.error());
    }
}

} // namespace straylight
