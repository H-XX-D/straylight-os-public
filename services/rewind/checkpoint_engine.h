/**
 * StrayLight Rewind — Checkpoint Engine
 *
 * Core process checkpointing: freeze via SIGSTOP, dump memory maps from
 * /proc/PID/maps, read memory via /proc/PID/mem, capture register state
 * (ptrace PTRACE_GETREGS concept), save FDs and sockets, resume with SIGCONT.
 *
 * Restore writes memory back via /proc/PID/mem. Full process restore
 * (including kernel state, threads, etc.) requires CRIU — this engine
 * provides the building blocks and a CRIU wrapper path.
 */
#pragma once

#include "checkpoint_store.h"
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
#include <sys/types.h>
#include <unordered_map>
#include <vector>

#ifdef __linux__
#include <dirent.h>
#include <signal.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace straylight::rewind {

// ── Configuration ───────────────────────────────────────────────────

struct EngineConfig {
    int checkpoint_interval_s   = 30;
    bool delta_checkpoints      = true;
    bool use_criu_for_restore   = false;  // when true, shell out to criu
    std::string criu_binary     = "/usr/sbin/criu";
    static constexpr size_t PAGE_SIZE = 4096;
};

// ── Engine ──────────────────────────────────────────────────────────

class CheckpointEngine {
public:
    explicit CheckpointEngine(CheckpointStore& store, EngineConfig config = {})
        : store_(store), config_(std::move(config)) {}

    /** Begin periodic checkpointing of a process. */
    VoidResult<> start_tracking(pid_t pid) {
        std::lock_guard lock(mu_);
        if (tracked_.count(pid))
            return VoidResult<>::error("already tracking pid " + std::to_string(pid));

        // Verify process exists
        if (!process_exists(pid))
            return VoidResult<>::error("process " + std::to_string(pid) + " does not exist");

        TrackedProcess tp;
        tp.pid = pid;
        tp.tracking_since = CheckpointStore::now_epoch_ms();
        tp.last_checkpoint_ms = 0;
        tp.auto_interval_s = config_.checkpoint_interval_s;
        tracked_[pid] = tp;
        return VoidResult<>::ok();
    }

    /** Stop tracking a process. */
    VoidResult<> stop_tracking(pid_t pid) {
        std::lock_guard lock(mu_);
        auto it = tracked_.find(pid);
        if (it == tracked_.end())
            return VoidResult<>::error("not tracking pid " + std::to_string(pid));
        tracked_.erase(it);
        return VoidResult<>::ok();
    }

    /** Check whether a PID is being tracked. */
    bool is_tracking(pid_t pid) const {
        std::lock_guard lock(mu_);
        return tracked_.count(pid) > 0;
    }

    /** Manual checkpoint creation — the core operation. */
    Result<std::string, std::string> create_checkpoint(pid_t pid) {
        if (!process_exists(pid))
            return Result<std::string, std::string>::error(
                "process " + std::to_string(pid) + " does not exist");

        Checkpoint cp;
        cp.meta.checkpoint_id = CheckpointStore::generate_id();
        cp.meta.pid = pid;
        cp.meta.timestamp = CheckpointStore::now_iso8601();
        cp.meta.epoch_ms = CheckpointStore::now_epoch_ms();

        // 1. Freeze process
        auto freeze_res = freeze_process(pid);
        if (!freeze_res)
            return Result<std::string, std::string>::error(freeze_res.err());

        // 2. Dump memory maps
        auto maps_res = read_memory_maps(pid);
        if (!maps_res) {
            resume_process(pid);
            return Result<std::string, std::string>::error(maps_res.err());
        }
        cp.regions = std::move(maps_res.value());

        // 3. Read memory contents for each region
        auto mem_res = read_memory_contents(pid, cp.regions);
        if (!mem_res) {
            resume_process(pid);
            return Result<std::string, std::string>::error(mem_res.err());
        }

        // 4. Capture register state
        cp.registers = capture_registers(pid);

        // 5. Save open file descriptors
        cp.file_descriptors = read_file_descriptors(pid);

        // 6. Save network socket state
        cp.sockets = read_socket_state(pid);

        // 7. Resume process
        resume_process(pid);

        // 8. Compute page hashes for delta support
        cp.page_hashes = compute_page_hashes(cp.regions);
        cp.meta.region_count = cp.regions.size();
        cp.meta.fd_count = cp.file_descriptors.size();
        cp.meta.socket_count = cp.sockets.size();

        // 9. Delta compression: strip unchanged pages
        if (config_.delta_checkpoints) {
            auto prev = store_.list(pid);
            if (!prev.empty()) {
                auto parent_id = prev.front().checkpoint_id;
                auto parent_res = store_.load(pid, parent_id);
                if (parent_res) {
                    apply_delta(cp, parent_res.value());
                }
            }
        }

        // 10. Store
        auto store_res = store_.save(cp);
        if (!store_res)
            return Result<std::string, std::string>::error(store_res.err());

        // Update tracking
        {
            std::lock_guard lock(mu_);
            auto it = tracked_.find(pid);
            if (it != tracked_.end()) {
                it->second.last_checkpoint_ms = cp.meta.epoch_ms;
                it->second.checkpoint_count++;
            }
        }

        return Result<std::string, std::string>::ok(cp.meta.checkpoint_id);
    }

    /** Restore process to a checkpoint. */
    VoidResult<> restore_checkpoint(pid_t pid, const std::string& checkpoint_id) {
        if (!process_exists(pid))
            return VoidResult<>::error("process " + std::to_string(pid) + " does not exist");

        // Load checkpoint
        auto cp_res = store_.load(pid, checkpoint_id);
        if (!cp_res)
            return VoidResult<>::error(cp_res.err());

        auto& cp = cp_res.value();

        // If this is a delta checkpoint, reconstruct full state
        if (cp.meta.is_delta && !cp.meta.parent_id.empty()) {
            auto parent_res = store_.load(pid, cp.meta.parent_id);
            if (!parent_res)
                return VoidResult<>::error("cannot load parent checkpoint: " + parent_res.err());
            reconstruct_from_delta(cp, parent_res.value());
        }

        // Use CRIU if configured
        if (config_.use_criu_for_restore) {
            return restore_via_criu(pid, checkpoint_id);
        }

        // Manual restore: write memory back
        auto freeze_res = freeze_process(pid);
        if (!freeze_res) return freeze_res;

        auto write_res = write_memory_contents(pid, cp.regions);
        if (!write_res) {
            resume_process(pid);
            return write_res;
        }

        // Restore registers (requires ptrace on Linux)
        restore_registers(pid, cp.registers);

        // Resume
        resume_process(pid);

        return VoidResult<>::ok();
    }

    /** List checkpoints for a PID. */
    std::vector<CheckpointMeta> list_checkpoints(pid_t pid) const {
        return store_.list(pid);
    }

    /** Called by the daemon tick — checks all tracked processes for auto-checkpoint. */
    void tick_auto_checkpoints() {
        std::vector<pid_t> due;
        {
            std::lock_guard lock(mu_);
            uint64_t now = CheckpointStore::now_epoch_ms();
            for (auto& [pid, tp] : tracked_) {
                if (!process_exists(pid)) {
                    tp.process_alive = false;
                    continue;
                }
                tp.process_alive = true;

                uint64_t interval_ms = static_cast<uint64_t>(tp.auto_interval_s) * 1000;
                if (tp.last_checkpoint_ms == 0 ||
                    (now - tp.last_checkpoint_ms) >= interval_ms) {
                    due.push_back(pid);
                }
            }
        }

        for (pid_t pid : due) {
            auto res = create_checkpoint(pid);
            if (!res) {
                fprintf(stderr, "[rewind] auto-checkpoint failed for pid %d: %s\n",
                        pid, res.err().c_str());
            }
        }
    }

    /** Remove dead processes from tracking. */
    void prune_dead_processes() {
        std::lock_guard lock(mu_);
        std::vector<pid_t> dead;
        for (auto& [pid, tp] : tracked_) {
            if (!process_exists(pid)) dead.push_back(pid);
        }
        for (pid_t p : dead) tracked_.erase(p);
    }

    /** Get all tracked processes and their state. */
    struct TrackingInfo {
        pid_t    pid           = 0;
        uint64_t tracking_since = 0;
        uint64_t last_checkpoint_ms = 0;
        int      auto_interval_s = 30;
        uint32_t checkpoint_count = 0;
        bool     process_alive = true;
    };

    std::vector<TrackingInfo> get_tracking_info() const {
        std::lock_guard lock(mu_);
        std::vector<TrackingInfo> info;
        info.reserve(tracked_.size());
        for (auto& [pid, tp] : tracked_) {
            TrackingInfo ti;
            ti.pid = pid;
            ti.tracking_since = tp.tracking_since;
            ti.last_checkpoint_ms = tp.last_checkpoint_ms;
            ti.auto_interval_s = tp.auto_interval_s;
            ti.checkpoint_count = tp.checkpoint_count;
            ti.process_alive = tp.process_alive;
            info.push_back(ti);
        }
        return info;
    }

private:
    CheckpointStore& store_;
    EngineConfig config_;
    mutable std::mutex mu_;

    struct TrackedProcess {
        pid_t    pid           = 0;
        uint64_t tracking_since = 0;
        uint64_t last_checkpoint_ms = 0;
        int      auto_interval_s = 30;
        uint32_t checkpoint_count = 0;
        bool     process_alive = true;
    };

    std::unordered_map<pid_t, TrackedProcess> tracked_;

    // ── Process manipulation ────────────────────────────────────────

    static bool process_exists(pid_t pid) {
#ifdef __linux__
        return kill(pid, 0) == 0;
#else
        // On non-Linux, check /proc existence concept
        return std::filesystem::exists("/proc/" + std::to_string(pid));
#endif
    }

    static VoidResult<> freeze_process(pid_t pid) {
#ifdef __linux__
        if (kill(pid, SIGSTOP) != 0)
            return VoidResult<>::error("SIGSTOP failed: " + std::string(strerror(errno)));
        // Wait for the process to actually stop
        int status;
        if (waitpid(pid, &status, WUNTRACED) == -1) {
            // If we can't wait (not our child), use ptrace attach
            if (ptrace(PTRACE_ATTACH, pid, nullptr, nullptr) == -1) {
                // SIGSTOP was sent; process should still stop
                usleep(10000); // 10ms grace period
            } else {
                waitpid(pid, &status, 0);
            }
        }
        return VoidResult<>::ok();
#else
        (void)pid;
        return VoidResult<>::ok(); // Cross-compile stub
#endif
    }

    static void resume_process(pid_t pid) {
#ifdef __linux__
        ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
        kill(pid, SIGCONT);
#else
        (void)pid;
#endif
    }

    // ── Memory maps ─────────────────────────────────────────────────

    static Result<std::vector<MemoryRegion>, std::string>
    read_memory_maps(pid_t pid) {
        std::vector<MemoryRegion> regions;
        auto maps_path = "/proc/" + std::to_string(pid) + "/maps";
        std::ifstream maps(maps_path);
        if (!maps)
            return Result<std::vector<MemoryRegion>, std::string>::error(
                "cannot open " + maps_path);

        std::string line;
        while (std::getline(maps, line)) {
            MemoryRegion region;
            // Format: start-end perms offset dev inode pathname
            std::istringstream iss(line);
            std::string addr_range, perms, offset_str, dev, inode;
            iss >> addr_range >> perms >> offset_str >> dev >> inode;

            // Rest of line is pathname
            std::string pathname;
            if (iss.peek() == ' ') iss.get();
            std::getline(iss, pathname);
            // Trim leading spaces
            auto first = pathname.find_first_not_of(' ');
            if (first != std::string::npos) pathname = pathname.substr(first);

            // Parse address range
            auto dash = addr_range.find('-');
            if (dash == std::string::npos) continue;

            try {
                region.start_addr = std::stoull(addr_range.substr(0, dash), nullptr, 16);
                region.end_addr   = std::stoull(addr_range.substr(dash + 1), nullptr, 16);
            } catch (...) { continue; }

            region.perms = perms;
            region.pathname = pathname;

            // Only capture readable, writable regions (skip shared libs code sections,
            // [vdso], [vsyscall], device mappings, etc.)
            if (perms.size() >= 2 && perms[0] == 'r' && perms[1] == 'w') {
                regions.push_back(std::move(region));
            }
        }

        return Result<std::vector<MemoryRegion>, std::string>::ok(std::move(regions));
    }

    static VoidResult<> read_memory_contents(pid_t pid,
                                              std::vector<MemoryRegion>& regions) {
        auto mem_path = "/proc/" + std::to_string(pid) + "/mem";
        std::ifstream mem(mem_path, std::ios::binary);
        if (!mem)
            return VoidResult<>::error("cannot open " + mem_path);

        for (auto& region : regions) {
            uint64_t size = region.end_addr - region.start_addr;
            // Cap region reads at 256MB to avoid OOM
            if (size > 256ULL * 1024 * 1024) {
                size = 256ULL * 1024 * 1024;
            }
            region.contents.resize(static_cast<size_t>(size));

            mem.seekg(static_cast<std::streamoff>(region.start_addr));
            if (!mem.good()) {
                // Some regions are not readable — fill with zeros
                std::fill(region.contents.begin(), region.contents.end(), 0);
                mem.clear();
                continue;
            }

            mem.read(reinterpret_cast<char*>(region.contents.data()),
                     static_cast<std::streamsize>(size));
            if (mem.fail()) {
                // Partial read is OK — zero-fill remainder
                auto got = static_cast<size_t>(mem.gcount());
                std::fill(region.contents.begin() + got, region.contents.end(), 0);
                mem.clear();
            }
        }

        return VoidResult<>::ok();
    }

    static VoidResult<> write_memory_contents(pid_t pid,
                                               const std::vector<MemoryRegion>& regions) {
        auto mem_path = "/proc/" + std::to_string(pid) + "/mem";
        std::ofstream mem(mem_path, std::ios::binary | std::ios::in);
        if (!mem)
            return VoidResult<>::error("cannot open " + mem_path + " for writing");

        for (auto& region : regions) {
            if (region.contents.empty()) continue;
            mem.seekp(static_cast<std::streamoff>(region.start_addr));
            if (!mem.good()) {
                mem.clear();
                continue;
            }
            mem.write(reinterpret_cast<const char*>(region.contents.data()),
                      static_cast<std::streamsize>(region.contents.size()));
            if (mem.fail()) {
                mem.clear();
                // Non-fatal: some regions may not be writable at restore time
            }
        }

        return VoidResult<>::ok();
    }

    // ── Register capture ────────────────────────────────────────────

    static RegisterState capture_registers(pid_t pid) {
        RegisterState regs{};
#ifdef __linux__
        struct user_regs_struct linux_regs{};
        if (ptrace(PTRACE_GETREGS, pid, nullptr, &linux_regs) == 0) {
            regs.rax = linux_regs.rax;
            regs.rbx = linux_regs.rbx;
            regs.rcx = linux_regs.rcx;
            regs.rdx = linux_regs.rdx;
            regs.rsi = linux_regs.rsi;
            regs.rdi = linux_regs.rdi;
            regs.rbp = linux_regs.rbp;
            regs.rsp = linux_regs.rsp;
            regs.r8  = linux_regs.r8;
            regs.r9  = linux_regs.r9;
            regs.r10 = linux_regs.r10;
            regs.r11 = linux_regs.r11;
            regs.r12 = linux_regs.r12;
            regs.r13 = linux_regs.r13;
            regs.r14 = linux_regs.r14;
            regs.r15 = linux_regs.r15;
            regs.rip = linux_regs.rip;
            regs.rflags = linux_regs.eflags;
            regs.cs  = linux_regs.cs;
            regs.ss  = linux_regs.ss;
            regs.ds  = linux_regs.ds;
            regs.es  = linux_regs.es;
            regs.fs  = linux_regs.fs;
            regs.gs  = linux_regs.gs;
            regs.fs_base = linux_regs.fs_base;
            regs.gs_base = linux_regs.gs_base;
        }
#else
        (void)pid;
#endif
        return regs;
    }

    static void restore_registers(pid_t pid, const RegisterState& regs) {
#ifdef __linux__
        struct user_regs_struct linux_regs{};
        // Read current regs first, then overlay our saved state
        ptrace(PTRACE_GETREGS, pid, nullptr, &linux_regs);
        linux_regs.rax = regs.rax;
        linux_regs.rbx = regs.rbx;
        linux_regs.rcx = regs.rcx;
        linux_regs.rdx = regs.rdx;
        linux_regs.rsi = regs.rsi;
        linux_regs.rdi = regs.rdi;
        linux_regs.rbp = regs.rbp;
        linux_regs.rsp = regs.rsp;
        linux_regs.r8  = regs.r8;
        linux_regs.r9  = regs.r9;
        linux_regs.r10 = regs.r10;
        linux_regs.r11 = regs.r11;
        linux_regs.r12 = regs.r12;
        linux_regs.r13 = regs.r13;
        linux_regs.r14 = regs.r14;
        linux_regs.r15 = regs.r15;
        linux_regs.rip = regs.rip;
        linux_regs.eflags = regs.rflags;
        linux_regs.cs  = regs.cs;
        linux_regs.ss  = regs.ss;
        linux_regs.ds  = regs.ds;
        linux_regs.es  = regs.es;
        linux_regs.fs  = regs.fs;
        linux_regs.gs  = regs.gs;
        linux_regs.fs_base = regs.fs_base;
        linux_regs.gs_base = regs.gs_base;
        ptrace(PTRACE_SETREGS, pid, nullptr, &linux_regs);
#else
        (void)pid;
        (void)regs;
#endif
    }

    // ── File descriptors ────────────────────────────────────────────

    static std::vector<FileDescriptorInfo> read_file_descriptors(pid_t pid) {
        std::vector<FileDescriptorInfo> fds;
        auto fd_dir = "/proc/" + std::to_string(pid) + "/fd";
        namespace fs = std::filesystem;
        std::error_code ec;

        if (!fs::exists(fd_dir, ec)) return fds;

        for (auto& entry : fs::directory_iterator(fd_dir, ec)) {
            FileDescriptorInfo info;
            try {
                info.fd = std::stoi(entry.path().filename().string());
            } catch (...) { continue; }

            // Read symlink to get the actual file path
            auto target = fs::read_symlink(entry.path(), ec);
            if (!ec) {
                info.path = target.string();
            }

            // Read fdinfo for offset and flags
            auto fdinfo_path = "/proc/" + std::to_string(pid) + "/fdinfo/"
                             + entry.path().filename().string();
            std::ifstream fdinfo(fdinfo_path);
            if (fdinfo) {
                std::string line;
                while (std::getline(fdinfo, line)) {
                    if (line.substr(0, 4) == "pos:") {
                        try { info.offset = std::stoull(line.substr(4)); }
                        catch (...) {}
                    } else if (line.substr(0, 6) == "flags:") {
                        try { info.flags = std::stoi(line.substr(6), nullptr, 8); }
                        catch (...) {}
                    }
                }
            }

            fds.push_back(std::move(info));
        }

        return fds;
    }

    // ── Socket state ────────────────────────────────────────────────

    static std::vector<SocketInfo> read_socket_state(pid_t pid) {
        std::vector<SocketInfo> sockets;

        // Read TCP sockets
        read_socket_file(pid, "/proc/" + std::to_string(pid) + "/net/tcp",
                        "tcp", sockets);
        // Read UDP sockets
        read_socket_file(pid, "/proc/" + std::to_string(pid) + "/net/udp",
                        "udp", sockets);
        // Read Unix sockets
        read_unix_sockets(pid, sockets);

        return sockets;
    }

    static void read_socket_file(pid_t pid, const std::string& path,
                                  const std::string& proto,
                                  std::vector<SocketInfo>& out) {
        (void)pid;
        std::ifstream f(path);
        if (!f) return;

        std::string line;
        std::getline(f, line); // skip header
        int fd_counter = 0;
        while (std::getline(f, line)) {
            std::istringstream iss(line);
            std::string sl, local, remote, state_hex;
            iss >> sl >> local >> remote >> state_hex;

            SocketInfo si;
            si.fd = fd_counter++;
            si.protocol = proto;
            si.local_addr = decode_hex_addr(local);
            si.remote_addr = decode_hex_addr(remote);
            si.state = decode_tcp_state(state_hex);
            out.push_back(std::move(si));
        }
    }

    static void read_unix_sockets(pid_t pid,
                                   std::vector<SocketInfo>& out) {
        auto path = "/proc/" + std::to_string(pid) + "/net/unix";
        std::ifstream f(path);
        if (!f) return;

        std::string line;
        std::getline(f, line); // skip header
        while (std::getline(f, line)) {
            std::istringstream iss(line);
            std::string num, refcount, proto, flags, type, state, inode;
            iss >> num >> refcount >> proto >> flags >> type >> state >> inode;
            std::string sock_path;
            iss >> sock_path;

            SocketInfo si;
            si.fd = -1;
            si.protocol = "unix";
            si.local_addr = sock_path;
            si.state = state;
            out.push_back(std::move(si));
        }
    }

    static std::string decode_hex_addr(const std::string& hex_addr) {
        // Format: AABBCCDD:PORT (hex IP:hex port)
        auto colon = hex_addr.find(':');
        if (colon == std::string::npos) return hex_addr;

        auto ip_hex = hex_addr.substr(0, colon);
        auto port_hex = hex_addr.substr(colon + 1);

        // Convert hex IP to dotted decimal (little-endian on x86)
        if (ip_hex.size() == 8) {
            try {
                uint32_t ip = std::stoul(ip_hex, nullptr, 16);
                uint16_t port = static_cast<uint16_t>(std::stoul(port_hex, nullptr, 16));
                return std::to_string(ip & 0xFF) + "."
                     + std::to_string((ip >> 8) & 0xFF) + "."
                     + std::to_string((ip >> 16) & 0xFF) + "."
                     + std::to_string((ip >> 24) & 0xFF)
                     + ":" + std::to_string(port);
            } catch (...) {}
        }
        return hex_addr;
    }

    static std::string decode_tcp_state(const std::string& hex) {
        int state = 0;
        try { state = std::stoi(hex, nullptr, 16); }
        catch (...) { return "UNKNOWN"; }
        switch (state) {
            case 1:  return "ESTABLISHED";
            case 2:  return "SYN_SENT";
            case 3:  return "SYN_RECV";
            case 4:  return "FIN_WAIT1";
            case 5:  return "FIN_WAIT2";
            case 6:  return "TIME_WAIT";
            case 7:  return "CLOSE";
            case 8:  return "CLOSE_WAIT";
            case 9:  return "LAST_ACK";
            case 10: return "LISTEN";
            case 11: return "CLOSING";
            default: return "UNKNOWN";
        }
    }

    // ── Delta checkpoint support ────────────────────────────────────

    std::vector<PageHash> compute_page_hashes(
            const std::vector<MemoryRegion>& regions) const {
        std::vector<PageHash> hashes;
        for (auto& region : regions) {
            size_t offset = 0;
            while (offset + EngineConfig::PAGE_SIZE <= region.contents.size()) {
                PageHash ph;
                ph.page_addr = region.start_addr + offset;
                ph.hash = CheckpointStore::hash_page(
                    region.contents.data() + offset, EngineConfig::PAGE_SIZE);
                hashes.push_back(ph);
                offset += EngineConfig::PAGE_SIZE;
            }
            // Trailing partial page
            if (offset < region.contents.size()) {
                PageHash ph;
                ph.page_addr = region.start_addr + offset;
                ph.hash = CheckpointStore::hash_page(
                    region.contents.data() + offset,
                    region.contents.size() - offset);
                hashes.push_back(ph);
            }
        }
        return hashes;
    }

    void apply_delta(Checkpoint& current, const Checkpoint& parent) const {
        // Build hash map from parent
        std::unordered_map<uint64_t, uint64_t> parent_hashes;
        for (auto& ph : parent.page_hashes) {
            parent_hashes[ph.page_addr] = ph.hash;
        }

        // For each region in current, zero out pages that haven't changed
        size_t pages_deduped = 0;
        for (auto& region : current.regions) {
            size_t offset = 0;
            while (offset + EngineConfig::PAGE_SIZE <= region.contents.size()) {
                uint64_t addr = region.start_addr + offset;
                uint64_t hash = CheckpointStore::hash_page(
                    region.contents.data() + offset, EngineConfig::PAGE_SIZE);

                auto it = parent_hashes.find(addr);
                if (it != parent_hashes.end() && it->second == hash) {
                    // Page unchanged — zero it out to compress well
                    std::fill(region.contents.begin() + offset,
                              region.contents.begin() + offset + EngineConfig::PAGE_SIZE,
                              0);
                    ++pages_deduped;
                }
                offset += EngineConfig::PAGE_SIZE;
            }
        }

        if (pages_deduped > 0) {
            current.meta.is_delta = true;
            current.meta.parent_id = parent.meta.checkpoint_id;
        }
    }

    void reconstruct_from_delta(Checkpoint& delta, const Checkpoint& parent) const {
        // For each zeroed page in delta, copy from parent
        std::unordered_map<uint64_t, const MemoryRegion*> parent_regions;
        for (auto& r : parent.regions) {
            parent_regions[r.start_addr] = &r;
        }

        for (auto& region : delta.regions) {
            auto it = parent_regions.find(region.start_addr);
            if (it == parent_regions.end()) continue;
            auto* pr = it->second;

            size_t offset = 0;
            while (offset + EngineConfig::PAGE_SIZE <= region.contents.size() &&
                   offset + EngineConfig::PAGE_SIZE <= pr->contents.size()) {
                // Check if this page is all zeros (was deduped)
                bool all_zero = true;
                for (size_t i = 0; i < EngineConfig::PAGE_SIZE && all_zero; ++i) {
                    if (region.contents[offset + i] != 0) all_zero = false;
                }
                if (all_zero) {
                    std::copy(pr->contents.begin() + offset,
                              pr->contents.begin() + offset + EngineConfig::PAGE_SIZE,
                              region.contents.begin() + offset);
                }
                offset += EngineConfig::PAGE_SIZE;
            }
        }
    }

    // ── CRIU wrapper ────────────────────────────────────────────────

    VoidResult<> restore_via_criu(pid_t pid, const std::string& checkpoint_id) const {
        (void)pid;
        // CRIU restore command:
        // criu restore --tree <pid> --images-dir <checkpoint_dir> --shell-job
        auto images_dir = store_.config().base_path + "/pid-"
                        + std::to_string(pid) + "/criu-" + checkpoint_id;

        std::string cmd = config_.criu_binary
            + " restore"
            + " --tree " + std::to_string(pid)
            + " --images-dir " + images_dir
            + " --shell-job 2>&1";

        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe)
            return VoidResult<>::error("cannot exec criu");

        char buffer[256];
        std::string output;
        while (fgets(buffer, sizeof(buffer), pipe)) {
            output += buffer;
        }
        int status = pclose(pipe);

        if (status != 0)
            return VoidResult<>::error("criu restore failed: " + output);

        return VoidResult<>::ok();
    }
};

} // namespace straylight::rewind
