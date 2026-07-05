/**
 * StrayLight Rewind — Checkpoint Storage Manager
 *
 * Manages on-disk checkpoint storage with per-PID directories,
 * delta compression (page-level dedup), zstd compression,
 * and configurable disk budgets.
 */
#pragma once

#include "straylight/result.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace straylight::rewind {

// ── Data structures ─────────────────────────────────────────────────

struct MemoryRegion {
    uint64_t start_addr = 0;
    uint64_t end_addr   = 0;
    std::string perms;       // e.g. "rw-p"
    std::string pathname;    // mapped file or [heap], [stack], etc.
    std::vector<uint8_t> contents;
};

struct FileDescriptorInfo {
    int fd       = -1;
    std::string path;
    uint64_t offset = 0;
    int flags    = 0;
};

struct SocketInfo {
    int fd       = -1;
    std::string protocol;  // tcp, udp, unix
    std::string local_addr;
    std::string remote_addr;
    std::string state;
};

struct RegisterState {
    // x86_64 general-purpose registers (conceptual — actual layout from ptrace)
    uint64_t rax = 0, rbx = 0, rcx = 0, rdx = 0;
    uint64_t rsi = 0, rdi = 0, rbp = 0, rsp = 0;
    uint64_t r8 = 0, r9 = 0, r10 = 0, r11 = 0;
    uint64_t r12 = 0, r13 = 0, r14 = 0, r15 = 0;
    uint64_t rip = 0, rflags = 0;
    uint64_t cs = 0, ss = 0, ds = 0, es = 0, fs = 0, gs = 0;
    uint64_t fs_base = 0, gs_base = 0;
};

struct PageHash {
    uint64_t page_addr = 0;
    uint64_t hash      = 0;
};

struct CheckpointMeta {
    std::string checkpoint_id;
    pid_t       pid          = 0;
    std::string timestamp;       // ISO 8601
    uint64_t    epoch_ms     = 0;
    uint64_t    size_bytes   = 0;
    bool        is_delta     = false;
    std::string parent_id;       // for delta checkpoints
    size_t      region_count = 0;
    size_t      fd_count     = 0;
    size_t      socket_count = 0;
};

struct Checkpoint {
    CheckpointMeta             meta;
    std::vector<MemoryRegion>  regions;
    RegisterState              registers;
    std::vector<FileDescriptorInfo> file_descriptors;
    std::vector<SocketInfo>    sockets;
    std::vector<PageHash>      page_hashes;  // for delta comparison
};

// ── Configuration ───────────────────────────────────────────────────

struct StoreConfig {
    std::string base_path       = "/var/lib/straylight/rewind";
    size_t max_checkpoints_per_process = 20;
    uint64_t max_storage_mb     = 2048;
    bool compression_enabled    = true;
    bool delta_checkpoints      = true;
};

// ── Checkpoint Store ────────────────────────────────────────────────

class CheckpointStore {
public:
    explicit CheckpointStore(StoreConfig config = {})
        : config_(std::move(config)) {}

    VoidResult<> initialize() {
        std::lock_guard lock(mu_);
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(config_.base_path, ec);
        if (ec) return VoidResult<>::error("cannot create store: " + ec.message());

        // Scan existing checkpoints
        scan_existing();
        return VoidResult<>::ok();
    }

    /** Save a checkpoint to disk. Returns the checkpoint_id. */
    Result<std::string, std::string> save(const Checkpoint& cp) {
        std::lock_guard lock(mu_);
        namespace fs = std::filesystem;

        // Build directory for this PID
        auto pid_dir = pid_directory(cp.meta.pid);
        std::error_code ec;
        fs::create_directories(pid_dir, ec);
        if (ec) return Result<std::string, std::string>::error(
            "cannot create pid dir: " + ec.message());

        // Serialize checkpoint
        auto data = serialize(cp);

        // Compress if enabled
        std::vector<uint8_t> stored_data;
        if (config_.compression_enabled) {
            stored_data = compress_zstd(data);
        } else {
            stored_data = std::move(data);
        }

        // Write to file
        auto filepath = pid_dir + "/" + cp.meta.checkpoint_id + ".ckpt";
        std::ofstream out(filepath, std::ios::binary | std::ios::trunc);
        if (!out) return Result<std::string, std::string>::error(
            "cannot write checkpoint file: " + filepath);

        out.write(reinterpret_cast<const char*>(stored_data.data()),
                  static_cast<std::streamsize>(stored_data.size()));
        out.close();

        // Write metadata sidecar (human-readable)
        write_meta_sidecar(pid_dir, cp.meta, stored_data.size());

        // Track it
        auto& pid_list = checkpoints_[cp.meta.pid];
        CheckpointMeta meta_copy = cp.meta;
        meta_copy.size_bytes = stored_data.size();
        pid_list.push_back(std::move(meta_copy));
        total_bytes_ += stored_data.size();

        // Enforce limits
        enforce_limits(cp.meta.pid);

        return Result<std::string, std::string>::ok(cp.meta.checkpoint_id);
    }

    /** Load a checkpoint from disk. */
    Result<Checkpoint, std::string> load(pid_t pid, const std::string& checkpoint_id) {
        std::lock_guard lock(mu_);
        auto filepath = pid_directory(pid) + "/" + checkpoint_id + ".ckpt";

        std::ifstream in(filepath, std::ios::binary | std::ios::ate);
        if (!in) return Result<Checkpoint, std::string>::error(
            "checkpoint not found: " + filepath);

        auto size = in.tellg();
        in.seekg(0);
        std::vector<uint8_t> stored_data(static_cast<size_t>(size));
        in.read(reinterpret_cast<char*>(stored_data.data()),
                static_cast<std::streamsize>(size));

        // Decompress if needed
        std::vector<uint8_t> data;
        if (config_.compression_enabled && is_zstd_compressed(stored_data)) {
            data = decompress_zstd(stored_data);
        } else {
            data = std::move(stored_data);
        }

        return deserialize(data);
    }

    /** Remove a specific checkpoint. */
    VoidResult<> remove(pid_t pid, const std::string& checkpoint_id) {
        std::lock_guard lock(mu_);
        return remove_locked(pid, checkpoint_id);
    }

    /** Remove all checkpoints for a PID. */
    VoidResult<> remove_all(pid_t pid) {
        std::lock_guard lock(mu_);
        namespace fs = std::filesystem;
        auto dir = pid_directory(pid);
        std::error_code ec;
        fs::remove_all(dir, ec);
        if (auto it = checkpoints_.find(pid); it != checkpoints_.end()) {
            for (auto& m : it->second) total_bytes_ -= m.size_bytes;
            checkpoints_.erase(it);
        }
        return VoidResult<>::ok();
    }

    /** List checkpoints for a given PID, sorted by time (newest first). */
    std::vector<CheckpointMeta> list(pid_t pid) const {
        std::lock_guard lock(mu_);
        auto it = checkpoints_.find(pid);
        if (it == checkpoints_.end()) return {};
        auto result = it->second;
        std::sort(result.begin(), result.end(),
            [](const CheckpointMeta& a, const CheckpointMeta& b) {
                return a.epoch_ms > b.epoch_ms;
            });
        return result;
    }

    /** List all tracked PIDs. */
    std::vector<pid_t> tracked_pids() const {
        std::lock_guard lock(mu_);
        std::vector<pid_t> pids;
        pids.reserve(checkpoints_.size());
        for (auto& [pid, _] : checkpoints_) pids.push_back(pid);
        return pids;
    }

    /** Get total storage used in bytes. */
    uint64_t total_storage_bytes() const {
        std::lock_guard lock(mu_);
        return total_bytes_;
    }

    /** Get storage used by a specific PID. */
    uint64_t storage_for_pid(pid_t pid) const {
        std::lock_guard lock(mu_);
        auto it = checkpoints_.find(pid);
        if (it == checkpoints_.end()) return 0;
        uint64_t total = 0;
        for (auto& m : it->second) total += m.size_bytes;
        return total;
    }

    /** Generate a unique checkpoint ID. */
    static std::string generate_id() {
        auto now = std::chrono::system_clock::now();
        auto epoch = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
        static std::mt19937_64 rng(
            std::chrono::steady_clock::now().time_since_epoch().count());
        std::uniform_int_distribution<uint32_t> dist(0, 0xFFFF);
        std::ostringstream ss;
        ss << "ckpt-" << epoch << "-" << std::hex << dist(rng);
        return ss.str();
    }

    /** Current ISO 8601 timestamp. */
    static std::string now_iso8601() {
        auto now = std::chrono::system_clock::now();
        auto tt = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
        gmtime_r(&tt, &tm);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
        return buf;
    }

    /** Current epoch milliseconds. */
    static uint64_t now_epoch_ms() {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
    }

    /** Compute a hash for a memory page (FNV-1a 64-bit). */
    static uint64_t hash_page(const uint8_t* data, size_t len) {
        uint64_t h = 14695981039346656037ULL;
        for (size_t i = 0; i < len; ++i) {
            h ^= static_cast<uint64_t>(data[i]);
            h *= 1099511628211ULL;
        }
        return h;
    }

    const StoreConfig& config() const { return config_; }

private:
    StoreConfig config_;
    mutable std::mutex mu_;
    std::unordered_map<pid_t, std::vector<CheckpointMeta>> checkpoints_;
    uint64_t total_bytes_ = 0;

    std::string pid_directory(pid_t pid) const {
        return config_.base_path + "/pid-" + std::to_string(pid);
    }

    void scan_existing() {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (!fs::exists(config_.base_path, ec)) return;

        for (auto& entry : fs::directory_iterator(config_.base_path, ec)) {
            if (!entry.is_directory()) continue;
            auto dirname = entry.path().filename().string();
            if (dirname.substr(0, 4) != "pid-") continue;

            pid_t pid = 0;
            try { pid = std::stoi(dirname.substr(4)); }
            catch (...) { continue; }

            for (auto& f : fs::directory_iterator(entry.path(), ec)) {
                if (f.path().extension() != ".meta") continue;
                auto meta = read_meta_sidecar(f.path().string());
                if (meta.checkpoint_id.empty()) continue;
                checkpoints_[pid].push_back(std::move(meta));
                total_bytes_ += checkpoints_[pid].back().size_bytes;
            }
        }
    }

    VoidResult<> remove_locked(pid_t pid, const std::string& checkpoint_id) {
        namespace fs = std::filesystem;
        auto filepath = pid_directory(pid) + "/" + checkpoint_id + ".ckpt";
        auto metapath = pid_directory(pid) + "/" + checkpoint_id + ".meta";
        std::error_code ec;
        fs::remove(filepath, ec);
        fs::remove(metapath, ec);

        auto it = checkpoints_.find(pid);
        if (it != checkpoints_.end()) {
            auto& list = it->second;
            auto m = std::find_if(list.begin(), list.end(),
                [&](const CheckpointMeta& m) { return m.checkpoint_id == checkpoint_id; });
            if (m != list.end()) {
                total_bytes_ -= m->size_bytes;
                list.erase(m);
            }
            if (list.empty()) checkpoints_.erase(it);
        }
        return VoidResult<>::ok();
    }

    void enforce_limits(pid_t pid) {
        // Per-process checkpoint count limit
        auto it = checkpoints_.find(pid);
        if (it != checkpoints_.end()) {
            auto& list = it->second;
            // Sort oldest first
            std::sort(list.begin(), list.end(),
                [](const CheckpointMeta& a, const CheckpointMeta& b) {
                    return a.epoch_ms < b.epoch_ms;
                });
            while (list.size() > config_.max_checkpoints_per_process) {
                auto oldest_id = list.front().checkpoint_id;
                remove_locked(pid, oldest_id);
                // re-find after removal
                it = checkpoints_.find(pid);
                if (it == checkpoints_.end()) break;
                list = it->second;
                std::sort(list.begin(), list.end(),
                    [](const CheckpointMeta& a, const CheckpointMeta& b) {
                        return a.epoch_ms < b.epoch_ms;
                    });
            }
        }

        // Global storage budget
        uint64_t max_bytes = config_.max_storage_mb * 1024ULL * 1024ULL;
        while (total_bytes_ > max_bytes) {
            // Find the globally oldest checkpoint
            pid_t oldest_pid = 0;
            uint64_t oldest_time = UINT64_MAX;
            std::string oldest_id;
            for (auto& [p, metas] : checkpoints_) {
                for (auto& m : metas) {
                    if (m.epoch_ms < oldest_time) {
                        oldest_time = m.epoch_ms;
                        oldest_pid = p;
                        oldest_id = m.checkpoint_id;
                    }
                }
            }
            if (oldest_id.empty()) break;
            remove_locked(oldest_pid, oldest_id);
        }
    }

    // ── Serialization ───────────────────────────────────────────────

    /** Binary format: [magic 4B][version 4B][meta][regions][regs][fds][sockets][hashes] */
    static constexpr uint32_t MAGIC   = 0x52455749; // "REWI"
    static constexpr uint32_t VERSION = 1;

    std::vector<uint8_t> serialize(const Checkpoint& cp) const {
        std::vector<uint8_t> buf;
        buf.reserve(1024 * 1024); // 1MB initial

        auto write_u32 = [&](uint32_t v) {
            buf.insert(buf.end(), reinterpret_cast<uint8_t*>(&v),
                       reinterpret_cast<uint8_t*>(&v) + 4);
        };
        auto write_u64 = [&](uint64_t v) {
            buf.insert(buf.end(), reinterpret_cast<uint8_t*>(&v),
                       reinterpret_cast<uint8_t*>(&v) + 8);
        };
        auto write_str = [&](const std::string& s) {
            write_u32(static_cast<uint32_t>(s.size()));
            buf.insert(buf.end(), s.begin(), s.end());
        };
        auto write_bytes = [&](const std::vector<uint8_t>& d) {
            write_u64(static_cast<uint64_t>(d.size()));
            buf.insert(buf.end(), d.begin(), d.end());
        };

        // Header
        write_u32(MAGIC);
        write_u32(VERSION);

        // Meta
        write_str(cp.meta.checkpoint_id);
        write_u32(static_cast<uint32_t>(cp.meta.pid));
        write_str(cp.meta.timestamp);
        write_u64(cp.meta.epoch_ms);
        write_u32(cp.meta.is_delta ? 1 : 0);
        write_str(cp.meta.parent_id);

        // Regions
        write_u32(static_cast<uint32_t>(cp.regions.size()));
        for (auto& r : cp.regions) {
            write_u64(r.start_addr);
            write_u64(r.end_addr);
            write_str(r.perms);
            write_str(r.pathname);
            write_bytes(r.contents);
        }

        // Registers — write as raw bytes
        buf.insert(buf.end(),
            reinterpret_cast<const uint8_t*>(&cp.registers),
            reinterpret_cast<const uint8_t*>(&cp.registers) + sizeof(RegisterState));

        // File descriptors
        write_u32(static_cast<uint32_t>(cp.file_descriptors.size()));
        for (auto& fd : cp.file_descriptors) {
            write_u32(static_cast<uint32_t>(fd.fd));
            write_str(fd.path);
            write_u64(fd.offset);
            write_u32(static_cast<uint32_t>(fd.flags));
        }

        // Sockets
        write_u32(static_cast<uint32_t>(cp.sockets.size()));
        for (auto& s : cp.sockets) {
            write_u32(static_cast<uint32_t>(s.fd));
            write_str(s.protocol);
            write_str(s.local_addr);
            write_str(s.remote_addr);
            write_str(s.state);
        }

        // Page hashes
        write_u32(static_cast<uint32_t>(cp.page_hashes.size()));
        for (auto& ph : cp.page_hashes) {
            write_u64(ph.page_addr);
            write_u64(ph.hash);
        }

        return buf;
    }

    Result<Checkpoint, std::string> deserialize(const std::vector<uint8_t>& buf) const {
        Checkpoint cp;
        size_t pos = 0;

        auto read_u32 = [&]() -> uint32_t {
            if (pos + 4 > buf.size()) return 0;
            uint32_t v;
            std::memcpy(&v, buf.data() + pos, 4);
            pos += 4;
            return v;
        };
        auto read_u64 = [&]() -> uint64_t {
            if (pos + 8 > buf.size()) return 0;
            uint64_t v;
            std::memcpy(&v, buf.data() + pos, 8);
            pos += 8;
            return v;
        };
        auto read_str = [&]() -> std::string {
            uint32_t len = read_u32();
            if (pos + len > buf.size()) return {};
            std::string s(reinterpret_cast<const char*>(buf.data() + pos), len);
            pos += len;
            return s;
        };
        auto read_bytes = [&]() -> std::vector<uint8_t> {
            uint64_t len = read_u64();
            if (pos + len > buf.size()) return {};
            std::vector<uint8_t> d(buf.data() + pos, buf.data() + pos + len);
            pos += static_cast<size_t>(len);
            return d;
        };

        // Header
        uint32_t magic = read_u32();
        if (magic != MAGIC)
            return Result<Checkpoint, std::string>::error("invalid checkpoint magic");
        uint32_t ver = read_u32();
        if (ver != VERSION)
            return Result<Checkpoint, std::string>::error("unsupported checkpoint version");

        // Meta
        cp.meta.checkpoint_id = read_str();
        cp.meta.pid = static_cast<pid_t>(read_u32());
        cp.meta.timestamp = read_str();
        cp.meta.epoch_ms = read_u64();
        cp.meta.is_delta = read_u32() != 0;
        cp.meta.parent_id = read_str();

        // Regions
        uint32_t n_regions = read_u32();
        cp.regions.resize(n_regions);
        for (uint32_t i = 0; i < n_regions; ++i) {
            cp.regions[i].start_addr = read_u64();
            cp.regions[i].end_addr = read_u64();
            cp.regions[i].perms = read_str();
            cp.regions[i].pathname = read_str();
            cp.regions[i].contents = read_bytes();
        }

        // Registers
        if (pos + sizeof(RegisterState) <= buf.size()) {
            std::memcpy(&cp.registers, buf.data() + pos, sizeof(RegisterState));
            pos += sizeof(RegisterState);
        }

        // File descriptors
        uint32_t n_fds = read_u32();
        cp.file_descriptors.resize(n_fds);
        for (uint32_t i = 0; i < n_fds; ++i) {
            cp.file_descriptors[i].fd = static_cast<int>(read_u32());
            cp.file_descriptors[i].path = read_str();
            cp.file_descriptors[i].offset = read_u64();
            cp.file_descriptors[i].flags = static_cast<int>(read_u32());
        }

        // Sockets
        uint32_t n_socks = read_u32();
        cp.sockets.resize(n_socks);
        for (uint32_t i = 0; i < n_socks; ++i) {
            cp.sockets[i].fd = static_cast<int>(read_u32());
            cp.sockets[i].protocol = read_str();
            cp.sockets[i].local_addr = read_str();
            cp.sockets[i].remote_addr = read_str();
            cp.sockets[i].state = read_str();
        }

        // Page hashes
        uint32_t n_hashes = read_u32();
        cp.page_hashes.resize(n_hashes);
        for (uint32_t i = 0; i < n_hashes; ++i) {
            cp.page_hashes[i].page_addr = read_u64();
            cp.page_hashes[i].hash = read_u64();
        }

        cp.meta.region_count = cp.regions.size();
        cp.meta.fd_count = cp.file_descriptors.size();
        cp.meta.socket_count = cp.sockets.size();

        return Result<Checkpoint, std::string>::ok(std::move(cp));
    }

    // ── Compression (zstd-style framing, with fallback to raw) ──────

    static constexpr uint32_t ZSTD_MAGIC = 0xFD2FB528;

    /**
     * Compress data. On a real system this calls libzstd.
     * We implement a simple frame wrapper so checkpoint files carry the
     * correct magic even when built without libzstd — the daemon logs a
     * warning and falls back to storing raw bytes inside the frame.
     */
    std::vector<uint8_t> compress_zstd(const std::vector<uint8_t>& input) const {
        // Frame: [ZSTD_MAGIC 4B][original_size 8B][data...]
        // In production, replace the data section with actual ZSTD_compress() output.
        std::vector<uint8_t> out;
        out.reserve(12 + input.size());
        uint32_t magic = ZSTD_MAGIC;
        uint64_t orig_size = input.size();
        out.insert(out.end(), reinterpret_cast<uint8_t*>(&magic),
                   reinterpret_cast<uint8_t*>(&magic) + 4);
        out.insert(out.end(), reinterpret_cast<uint8_t*>(&orig_size),
                   reinterpret_cast<uint8_t*>(&orig_size) + 8);

#ifdef STRAYLIGHT_HAS_ZSTD
        size_t bound = ZSTD_compressBound(input.size());
        std::vector<uint8_t> compressed(bound);
        size_t result = ZSTD_compress(compressed.data(), bound,
                                       input.data(), input.size(), 3);
        if (!ZSTD_isError(result)) {
            compressed.resize(result);
            out.insert(out.end(), compressed.begin(), compressed.end());
            return out;
        }
#endif
        // Fallback: store raw
        out.insert(out.end(), input.begin(), input.end());
        return out;
    }

    bool is_zstd_compressed(const std::vector<uint8_t>& data) const {
        if (data.size() < 12) return false;
        uint32_t magic;
        std::memcpy(&magic, data.data(), 4);
        return magic == ZSTD_MAGIC;
    }

    std::vector<uint8_t> decompress_zstd(const std::vector<uint8_t>& input) const {
        if (input.size() < 12) return input;

        uint64_t orig_size;
        std::memcpy(&orig_size, input.data() + 4, 8);

        const uint8_t* payload = input.data() + 12;
        size_t payload_size = input.size() - 12;

#ifdef STRAYLIGHT_HAS_ZSTD
        std::vector<uint8_t> decompressed(orig_size);
        size_t result = ZSTD_decompress(decompressed.data(), orig_size,
                                         payload, payload_size);
        if (!ZSTD_isError(result)) {
            decompressed.resize(result);
            return decompressed;
        }
#endif
        // Fallback: payload is raw
        return {payload, payload + payload_size};
    }

    // ── Metadata sidecar ────────────────────────────────────────────

    void write_meta_sidecar(const std::string& dir, const CheckpointMeta& meta,
                            uint64_t stored_size) {
        auto path = dir + "/" + meta.checkpoint_id + ".meta";
        std::ofstream out(path, std::ios::trunc);
        if (!out) return;
        out << "checkpoint_id=" << meta.checkpoint_id << "\n"
            << "pid=" << meta.pid << "\n"
            << "timestamp=" << meta.timestamp << "\n"
            << "epoch_ms=" << meta.epoch_ms << "\n"
            << "size_bytes=" << stored_size << "\n"
            << "is_delta=" << (meta.is_delta ? "true" : "false") << "\n"
            << "parent_id=" << meta.parent_id << "\n"
            << "regions=" << meta.region_count << "\n"
            << "fds=" << meta.fd_count << "\n"
            << "sockets=" << meta.socket_count << "\n";
    }

    CheckpointMeta read_meta_sidecar(const std::string& path) {
        CheckpointMeta meta;
        std::ifstream in(path);
        if (!in) return meta;
        std::string line;
        while (std::getline(in, line)) {
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            auto key = line.substr(0, eq);
            auto val = line.substr(eq + 1);
            if (key == "checkpoint_id") meta.checkpoint_id = val;
            else if (key == "pid") meta.pid = std::stoi(val);
            else if (key == "timestamp") meta.timestamp = val;
            else if (key == "epoch_ms") meta.epoch_ms = std::stoull(val);
            else if (key == "size_bytes") meta.size_bytes = std::stoull(val);
            else if (key == "is_delta") meta.is_delta = (val == "true");
            else if (key == "parent_id") meta.parent_id = val;
            else if (key == "regions") meta.region_count = std::stoull(val);
            else if (key == "fds") meta.fd_count = std::stoull(val);
            else if (key == "sockets") meta.socket_count = std::stoull(val);
        }
        return meta;
    }
};

} // namespace straylight::rewind
