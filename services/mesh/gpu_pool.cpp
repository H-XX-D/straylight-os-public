// services/mesh/gpu_pool.cpp
// StrayLight Mesh — GPU pool implementation.
#include "gpu_pool.h"

#include <straylight/log.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <exception>
#include <fstream>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace straylight {

using json = nlohmann::json;

static constexpr const char* VPU_DEVICE_PATH = "/dev/straylight-vpu";
static constexpr const char* VPU_SLAB_USAGE_PATH = "/sys/kernel/straylight-vpu/slab_usage";
static constexpr const char* REMOTE_CLI_PATH = "/usr/local/bin/straylight-remote";
static constexpr const char* SWARM_CLI_PATH = "/usr/local/bin/straylight-swarm-cli";
static constexpr const char* OS_STATUS_CACHE_PATH = "/run/straylight/os-status.json";
static constexpr const char* VPU_REMOTE_HELPER = "/usr/local/bin/straylight-vpu-remote-buffer";

#define VPU_IOC_MAGIC 'V'

struct VpuAllocRequest {
    uint64_t size;      // in
    uint32_t flags;     // in
    uint32_t pad;
    uint64_t handle;    // out
    uint64_t gpu_addr;  // out
};

struct VpuFreeRequest {
    uint64_t handle;
};

struct VpuQueryRequest {
    uint64_t handle;    // in
    uint64_t size;      // out
    uint32_t order;     // out
    uint32_t flags;     // out
    uint64_t gpu_addr;  // out
};

struct VpuDmaExportRequest {
    uint64_t handle;    // in
    int32_t fd;         // out
    uint32_t flags;     // in
};

struct VpuStats {
    size_t total_bytes = 0;
    size_t free_bytes = 0;
    float utilization = 0.0f;
};

struct CachedRemoteStats {
    size_t total_bytes = 0;
    size_t free_bytes = 0;
    float temperature = 0.0f;
    float utilization = 0.0f;
};

#define VPU_IOC_ALLOC _IOWR(VPU_IOC_MAGIC, 0x01, VpuAllocRequest)
#define VPU_IOC_FREE  _IOW(VPU_IOC_MAGIC, 0x02, VpuFreeRequest)
#define VPU_IOC_QUERY _IOWR(VPU_IOC_MAGIC, 0x04, VpuQueryRequest)
#define VPU_IOC_EXPORT_DMA _IOWR(VPU_IOC_MAGIC, 0x05, VpuDmaExportRequest)

// ---------------------------------------------------------------------------
// Helper: run a command and capture stdout
// ---------------------------------------------------------------------------

static std::pair<std::string, bool> run_command(const std::string& cmd) {
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return {"", false};

    std::string output;
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) {
        output += buf;
    }
    int status = pclose(pipe);
    return {output, status == 0};
}

static std::string shell_quote(const std::string& value) {
    std::string out = "'";
    for (char c : value) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    out += "'";
    return out;
}

static std::string remote_key_path() {
    if (const char* explicit_path = std::getenv("STRAYLIGHT_REMOTE_KEY_PATH")) {
        if (*explicit_path) return explicit_path;
    }
    if (const char* config_home = std::getenv("XDG_CONFIG_HOME")) {
        if (*config_home) return std::string(config_home) + "/straylight/remote/id_ed25519";
    }
    if (const char* home = std::getenv("HOME")) {
        if (*home) return std::string(home) + "/.config/straylight/remote/id_ed25519";
    }
    return "/etc/straylight/remote/id_ed25519";
}

static std::string trim_copy(const std::string& value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

static std::string temp_path(const std::string& prefix) {
    const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return "/tmp/" + prefix + "-" + std::to_string(getpid()) + "-" +
           std::to_string(now) + ".bin";
}

static Result<void, std::string> run_checked(const std::string& cmd,
                                             const std::string& context) {
    auto [output, ok] = run_command(cmd + " 2>&1");
    if (!ok) {
        return Result<void, std::string>::error(context + ": " + output);
    }
    return Result<void, std::string>::ok();
}

static Result<void, std::string> remote_download_file(
    const std::string& host, const std::string& remote_path,
    const std::string& local_path) {
    return run_checked(
        std::string(REMOTE_CLI_PATH) + " download --key " +
        shell_quote(remote_key_path()) + " " + shell_quote(host) + " " +
        shell_quote(remote_path) + " " + shell_quote(local_path),
        "remote download failed from " + host + ":" + remote_path);
}

static Result<void, std::string> remote_upload_file(
    const std::string& host, const std::string& local_path,
    const std::string& remote_path) {
    return run_checked(
        std::string(REMOTE_CLI_PATH) + " upload --key " +
        shell_quote(remote_key_path()) + " " + shell_quote(host) + " " +
        shell_quote(local_path) + " " + shell_quote(remote_path),
        "remote upload failed to " + host + ":" + remote_path);
}

static bool parse_size_token(const std::string& token,
                             const char* prefix,
                             size_t& value) {
    const size_t prefix_len = std::strlen(prefix);
    if (token.rfind(prefix, 0) != 0) return false;

    try {
        value = static_cast<size_t>(std::stoull(token.substr(prefix_len)));
        return true;
    } catch (...) {
        return false;
    }
}

static bool parse_used_token(const std::string& token,
                             size_t& used,
                             size_t& blocks) {
    static constexpr const char* prefix = "used=";
    const size_t prefix_len = std::strlen(prefix);
    if (token.rfind(prefix, 0) != 0) return false;

    const std::string value = token.substr(prefix_len);
    const size_t slash = value.find('/');
    if (slash == std::string::npos) return false;

    try {
        used = static_cast<size_t>(std::stoull(value.substr(0, slash)));
        blocks = static_cast<size_t>(std::stoull(value.substr(slash + 1)));
        return true;
    } catch (...) {
        return false;
    }
}

static bool read_vpu_stats(VpuStats& stats) {
    std::ifstream in(VPU_SLAB_USAGE_PATH);
    if (!in.is_open()) return false;

    VpuStats next;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream iss(line);
        std::string token;
        size_t block_size = 0;
        size_t used = 0;
        size_t blocks = 0;
        bool have_block_size = false;
        bool have_used = false;

        while (iss >> token) {
            have_block_size =
                parse_size_token(token, "block_size=", block_size) ||
                have_block_size;
            have_used = parse_used_token(token, used, blocks) || have_used;
        }

        if (!have_block_size || !have_used || used > blocks) continue;
        next.total_bytes += block_size * blocks;
        next.free_bytes += block_size * (blocks - used);
    }

    if (next.total_bytes == 0) return false;
    next.utilization =
        1.0f - (static_cast<float>(next.free_bytes) /
                static_cast<float>(next.total_bytes));
    stats = next;
    return true;
}

static std::unordered_map<std::string, CachedRemoteStats> read_cached_remote_stats() {
    std::unordered_map<std::string, CachedRemoteStats> result;

    std::ifstream cache(OS_STATUS_CACHE_PATH);
    if (!cache.is_open()) return result;

    try {
        json os_status = json::parse(cache);
        if (!os_status.contains("swarm") ||
            !os_status["swarm"].contains("snapshot") ||
            !os_status["swarm"]["snapshot"].contains("nodes") ||
            !os_status["swarm"]["snapshot"]["nodes"].is_array()) {
            return result;
        }

        for (const auto& entry : os_status["swarm"]["snapshot"]["nodes"]) {
            if (!entry.value("online", false)) continue;
            if (!entry.value("gpu_present", false)) continue;

            const json node = entry.value("node", json::object());
            const std::string host = node.value("host", "");
            if (host.empty()) continue;

            const json vpu = entry.value("vpu", json::object());
            if (!vpu.value("ready", false)) continue;

            const uint64_t total = vpu.value("capacity_bytes", static_cast<uint64_t>(0));
            const uint64_t free = vpu.value("free_bytes", static_cast<uint64_t>(0));
            if (total == 0) continue;

            CachedRemoteStats stats;
            stats.total_bytes = static_cast<size_t>(total);
            stats.free_bytes = static_cast<size_t>(free);
            stats.temperature = static_cast<float>(entry.value("max_temp_c", 0.0));
            stats.utilization = 1.0f -
                (static_cast<float>(free) / static_cast<float>(total));
            result[host] = stats;
        }
    } catch (const std::exception& e) {
        SL_WARN("mesh: failed to parse cached remote stats at {}: {}",
                OS_STATUS_CACHE_PATH, e.what());
    }

    return result;
}

static int open_vpu_device() {
    return open(VPU_DEVICE_PATH, O_RDWR);
}

static Result<VpuQueryRequest, std::string> query_vpu_handle(int fd, uint64_t handle) {
    VpuQueryRequest req{};
    req.handle = handle;
    int ret = ioctl(fd, VPU_IOC_QUERY, &req);
    int saved_errno = errno;
    if (ret != 0) {
        return Result<VpuQueryRequest, std::string>::error(
            "VPU_IOC_QUERY failed for handle " + std::to_string(handle) +
            ": " + strerror(saved_errno));
    }
    return Result<VpuQueryRequest, std::string>::ok(req);
}

static Result<int, std::string> export_vpu_dmabuf(int fd, uint64_t handle) {
    VpuDmaExportRequest req{};
    req.handle = handle;
    req.fd = -1;
    req.flags = O_RDWR | O_CLOEXEC;
    int ret = ioctl(fd, VPU_IOC_EXPORT_DMA, &req);
    int saved_errno = errno;
    if (ret != 0) {
        return Result<int, std::string>::error(
            "VPU_IOC_EXPORT_DMA failed for handle " + std::to_string(handle) +
            ": " + strerror(saved_errno));
    }
    return Result<int, std::string>::ok(req.fd);
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

GpuPool::GpuPool()  = default;
GpuPool::~GpuPool() = default;

// ---------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------

Result<void, std::string> GpuPool::discover() {
    std::lock_guard<std::mutex> lock(mutex_);
    gpus_.clear();

    auto local_result = discover_local();
    if (!local_result.has_value()) {
        SL_WARN("mesh: local GPU discovery failed: {}", local_result.error());
    }

    auto remote_result = discover_remote();
    if (!remote_result.has_value()) {
        SL_WARN("mesh: remote GPU discovery failed: {}", remote_result.error());
    }

    SL_INFO("mesh: discovered {} GPU(s) total", gpus_.size());
    return Result<void, std::string>::ok();
}

Result<void, std::string> GpuPool::discover_local() {
    int fd = open_vpu_device();
    if (fd < 0) {
        SL_INFO("mesh: no local VPU device found at {}", VPU_DEVICE_PATH);
        return Result<void, std::string>::ok();
    }
    close(fd);

    VpuStats stats;
    if (!read_vpu_stats(stats)) {
        SL_WARN("mesh: local VPU present but stats unavailable at {}",
                VPU_SLAB_USAGE_PATH);
        return Result<void, std::string>::ok();
    }

    RemoteGpu gpu;
    gpu.host           = "localhost";
    gpu.gpu_index      = 0;
    gpu.name           = "straylight-vpu";
    gpu.vendor         = "straylight";
    gpu.vram_total     = stats.total_bytes;
    gpu.vram_available = stats.free_bytes;
    gpu.temperature    = 0.0f;
    gpu.utilization    = stats.utilization;
    gpu.latency_ms     = 0.0f;
    gpu.is_local       = true;
    gpu.is_available   = true;
    gpu.last_seen      = std::chrono::steady_clock::now();
    gpus_.push_back(std::move(gpu));

    SL_INFO("mesh: found 1 local VPU at {} (free={}MiB total={}MiB)",
            VPU_DEVICE_PATH,
            stats.free_bytes / (1024 * 1024),
            stats.total_bytes / (1024 * 1024));

    return Result<void, std::string>::ok();
}

Result<void, std::string> GpuPool::discover_remote() {
    try {
        json snapshot;
        bool have_snapshot = false;

        std::ifstream cache(OS_STATUS_CACHE_PATH);
        if (cache.is_open()) {
            try {
                json os_status = json::parse(cache);
                if (os_status.contains("swarm") &&
                    os_status["swarm"].contains("snapshot") &&
                    os_status["swarm"]["snapshot"].contains("nodes")) {
                    snapshot = os_status["swarm"]["snapshot"];
                    have_snapshot = true;
                }
            } catch (const std::exception& e) {
                SL_WARN("mesh: failed to parse cached OS status at {}: {}",
                        OS_STATUS_CACHE_PATH, e.what());
            }
        }

        if (!have_snapshot) {
            auto [output, ok] = run_command(
                "timeout 8s " + std::string(SWARM_CLI_PATH) + " snapshot 2>/dev/null");

            if (!ok || output.empty()) {
                SL_DEBUG("mesh: swarm snapshot not available, skipping remote discovery");
                return Result<void, std::string>::ok();
            }
            snapshot = json::parse(output);
            have_snapshot = true;
        }

        if (!snapshot.contains("nodes") || !snapshot["nodes"].is_array()) {
            return Result<void, std::string>::ok();
        }

        size_t discovered = 0;
        for (const auto& entry : snapshot["nodes"]) {
            if (!entry.value("online", false)) continue;
            if (!entry.value("gpu_present", false)) continue;

            const json node = entry.value("node", json::object());
            const std::string host = node.value("host", "");
            if (host.empty() || host == "localhost" || host == "127.0.0.1") continue;

            const json vpu = entry.value("vpu", json::object());
            if (!vpu.value("ready", false)) continue;

            const uint64_t total = vpu.value("capacity_bytes", static_cast<uint64_t>(0));
            const uint64_t free = vpu.value("free_bytes", static_cast<uint64_t>(0));
            if (total == 0) continue;

            const std::string hostname = entry.value("hostname", node.value("name", host));
            RemoteGpu gpu;
            gpu.host           = host;
            gpu.gpu_index      = 0;
            gpu.name           = hostname + "-jetson-vpu";
            gpu.vendor         = "nvidia-jetson";
            gpu.vram_total     = static_cast<size_t>(total);
            gpu.vram_available = static_cast<size_t>(free);
            gpu.temperature    = static_cast<float>(entry.value("max_temp_c", 0.0));
            gpu.utilization    = 1.0f - (static_cast<float>(free) / static_cast<float>(total));
            gpu.latency_ms     = static_cast<float>(entry.value("probe_elapsed_sec", 0.0) * 1000.0);
            gpu.is_local       = false;
            gpu.is_available   = true;
            gpu.last_seen      = std::chrono::steady_clock::now();
            gpus_.push_back(std::move(gpu));
            discovered++;
        }

        if (discovered > 0) {
            SL_INFO("mesh: found {} remote Jetson GPU/VPU peer(s) from swarm snapshot",
                    discovered);
        }
    } catch (const std::exception& e) {
        return Result<void, std::string>::error(
            "Failed to parse swarm snapshot JSON: " + std::string(e.what()));
    }

    return Result<void, std::string>::ok();
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

std::vector<RemoteGpu> GpuPool::all_gpus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return gpus_;
}

std::vector<RemoteGpu> GpuPool::available_gpus(float max_utilization) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<RemoteGpu> result;
    for (const auto& gpu : gpus_) {
        if (gpu.is_available && gpu.utilization < max_utilization) {
            result.push_back(gpu);
        }
    }
    return result;
}

size_t GpuPool::gpu_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return gpus_.size();
}

void GpuPool::mesh_totals(size_t& total, size_t& available) const {
    std::lock_guard<std::mutex> lock(mutex_);
    total = 0;
    available = 0;
    for (const auto& gpu : gpus_) {
        if (gpu.is_available) {
            total     += gpu.vram_total;
            available += gpu.vram_available;
        }
    }
}

// ---------------------------------------------------------------------------
// GPU selection
// ---------------------------------------------------------------------------

Result<RemoteGpu*, std::string> GpuPool::select_gpu(size_t bytes, PlacementPolicy policy) {
    std::vector<RemoteGpu*> candidates;
    for (auto& gpu : gpus_) {
        if (gpu.is_available && gpu.vram_available >= bytes) {
            candidates.push_back(&gpu);
        }
    }

    if (candidates.empty()) {
        return Result<RemoteGpu*, std::string>::error(
            "No GPU with sufficient VRAM available (" +
            std::to_string(bytes / (1024 * 1024)) + " MiB requested)");
    }

    RemoteGpu* selected = nullptr;

    switch (policy) {
        case PlacementPolicy::BestFit:
            selected = *std::min_element(candidates.begin(), candidates.end(),
                [](const RemoteGpu* a, const RemoteGpu* b) {
                    return a->vram_available < b->vram_available;
                });
            break;

        case PlacementPolicy::LeastLoaded:
            selected = *std::min_element(candidates.begin(), candidates.end(),
                [](const RemoteGpu* a, const RemoteGpu* b) {
                    return a->utilization < b->utilization;
                });
            break;

        case PlacementPolicy::LocalFirst: {
            std::vector<RemoteGpu*> local, remote;
            for (auto* g : candidates) {
                if (g->is_local) local.push_back(g);
                else              remote.push_back(g);
            }
            auto& pool = local.empty() ? remote : local;
            selected = *std::min_element(pool.begin(), pool.end(),
                [](const RemoteGpu* a, const RemoteGpu* b) {
                    return a->utilization < b->utilization;
                });
            break;
        }

        case PlacementPolicy::RoundRobin:
            selected = candidates[round_robin_index_ % candidates.size()];
            round_robin_index_++;
            break;

        case PlacementPolicy::Pinned:
            selected = candidates[0];
            break;
    }

    return Result<RemoteGpu*, std::string>::ok(selected);
}

// ---------------------------------------------------------------------------
// Allocation
// ---------------------------------------------------------------------------

Result<uint64_t, std::string> GpuPool::local_alloc(uint32_t gpu_index, size_t bytes) {
    (void)gpu_index;
    int fd = open_vpu_device();
    if (fd < 0) {
        return Result<uint64_t, std::string>::error(
            "Cannot open " + std::string(VPU_DEVICE_PATH) + ": " + strerror(errno));
    }

    VpuAllocRequest req{};
    req.size = bytes;
    req.flags = 0;
    int ret = ioctl(fd, VPU_IOC_ALLOC, &req);
    int saved_errno = errno;
    close(fd);

    if (ret != 0) {
        return Result<uint64_t, std::string>::error(
            "VPU_IOC_ALLOC failed: " + std::string(strerror(saved_errno)));
    }

    return Result<uint64_t, std::string>::ok(req.handle);
}

Result<uint64_t, std::string> GpuPool::remote_alloc(const std::string& host,
                                                      uint32_t gpu_index, size_t bytes) {
    (void)gpu_index;
    auto exec_result = remote_exec(
        host, std::string(VPU_REMOTE_HELPER) + " alloc " + std::to_string(bytes));
    if (!exec_result.has_value()) {
        return Result<uint64_t, std::string>::error(exec_result.error());
    }

    const std::string output = trim_copy(exec_result.value());
    try {
        return Result<uint64_t, std::string>::ok(
            static_cast<uint64_t>(std::stoull(output, nullptr, 0)));
    } catch (...) {
        return Result<uint64_t, std::string>::error(
            "Cannot parse handle from remote alloc on " + host + ": " + output);
    }
}

Result<MeshAllocation, std::string> GpuPool::allocate(size_t bytes, PlacementPolicy policy) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto gpu_result = select_gpu(bytes, policy);
    if (!gpu_result.has_value()) {
        return Result<MeshAllocation, std::string>::error(gpu_result.error());
    }

    RemoteGpu* gpu = gpu_result.value();

    uint64_t handle = 0;
    if (gpu->is_local) {
        auto alloc_result = local_alloc(gpu->gpu_index, bytes);
        if (!alloc_result.has_value()) {
            return Result<MeshAllocation, std::string>::error(alloc_result.error());
        }
        handle = alloc_result.value();
    } else {
        auto alloc_result = remote_alloc(gpu->host, gpu->gpu_index, bytes);
        if (!alloc_result.has_value()) {
            return Result<MeshAllocation, std::string>::error(alloc_result.error());
        }
        handle = alloc_result.value();
    }

    if (gpu->vram_available >= bytes) {
        gpu->vram_available -= bytes;
    }

    MeshAllocation alloc;
    alloc.handle     = handle;
    alloc.host       = gpu->host;
    alloc.gpu_index  = gpu->gpu_index;
    alloc.size_bytes = bytes;
    alloc.is_local   = gpu->is_local;

    SL_DEBUG("mesh: allocated {} bytes on {}:gpu{} (handle={})",
             bytes, gpu->host, gpu->gpu_index, handle);

    return Result<MeshAllocation, std::string>::ok(alloc);
}

// ---------------------------------------------------------------------------
// Free
// ---------------------------------------------------------------------------

Result<void, std::string> GpuPool::local_free(uint32_t gpu_index, uint64_t handle) {
    (void)gpu_index;
    int fd = open_vpu_device();
    if (fd < 0) {
        return Result<void, std::string>::error(
            "Cannot open " + std::string(VPU_DEVICE_PATH) + ": " + strerror(errno));
    }

    VpuFreeRequest req{};
    req.handle = handle;
    int ret = ioctl(fd, VPU_IOC_FREE, &req);
    int saved_errno = errno;
    close(fd);

    if (ret != 0) {
        return Result<void, std::string>::error(
            "VPU_IOC_FREE failed: " + std::string(strerror(saved_errno)));
    }

    return Result<void, std::string>::ok();
}

Result<void, std::string> GpuPool::remote_free(const std::string& host,
                                                 uint32_t gpu_index, uint64_t handle) {
    (void)gpu_index;
    auto exec_result = remote_exec(host, std::string(VPU_REMOTE_HELPER) + " free " +
                                         std::to_string(handle));
    if (!exec_result.has_value()) {
        return Result<void, std::string>::error(exec_result.error());
    }
    return Result<void, std::string>::ok();
}

Result<void, std::string> GpuPool::free(const MeshAllocation& alloc) {
    std::lock_guard<std::mutex> lock(mutex_);

    Result<void, std::string> result = alloc.is_local
        ? local_free(alloc.gpu_index, alloc.handle)
        : remote_free(alloc.host, alloc.gpu_index, alloc.handle);

    if (result.has_value()) {
        for (auto& gpu : gpus_) {
            if (gpu.host == alloc.host && gpu.gpu_index == alloc.gpu_index) {
                gpu.vram_available += alloc.size_bytes;
                break;
            }
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// Transfer
// ---------------------------------------------------------------------------

Result<void, std::string> GpuPool::local_to_local_transfer(
    uint32_t src_gpu, uint64_t src_handle,
    uint32_t dst_gpu, uint64_t dst_handle, size_t bytes)
{
    (void)src_gpu;
    (void)dst_gpu;

    if (bytes == 0) {
        return Result<void, std::string>::ok();
    }

    int fd = open_vpu_device();
    if (fd < 0) {
        return Result<void, std::string>::error(
            "Cannot open " + std::string(VPU_DEVICE_PATH) + ": " + strerror(errno));
    }

    auto src_query = query_vpu_handle(fd, src_handle);
    if (!src_query.has_value()) {
        close(fd);
        return Result<void, std::string>::error(src_query.error());
    }
    auto dst_query = query_vpu_handle(fd, dst_handle);
    if (!dst_query.has_value()) {
        close(fd);
        return Result<void, std::string>::error(dst_query.error());
    }

    if (src_query.value().size < bytes || dst_query.value().size < bytes) {
        close(fd);
        return Result<void, std::string>::error(
            "Local VPU transfer size exceeds allocation: src=" +
            std::to_string(src_query.value().size) +
            " dst=" + std::to_string(dst_query.value().size) +
            " requested=" + std::to_string(bytes));
    }

    auto src_export = export_vpu_dmabuf(fd, src_handle);
    if (!src_export.has_value()) {
        close(fd);
        return Result<void, std::string>::error(src_export.error());
    }

    auto dst_export = export_vpu_dmabuf(fd, dst_handle);
    if (!dst_export.has_value()) {
        close(src_export.value());
        close(fd);
        return Result<void, std::string>::error(dst_export.error());
    }

    const int src_dma_fd = src_export.value();
    const int dst_dma_fd = dst_export.value();
    void* src_map = mmap(nullptr, src_query.value().size,
                         PROT_READ | PROT_WRITE, MAP_SHARED, src_dma_fd, 0);
    if (src_map == MAP_FAILED) {
        int saved_errno = errno;
        close(dst_dma_fd);
        close(src_dma_fd);
        close(fd);
        return Result<void, std::string>::error(
            "mmap source dma-buf failed: " + std::string(strerror(saved_errno)));
    }

    void* dst_map = mmap(nullptr, dst_query.value().size,
                         PROT_READ | PROT_WRITE, MAP_SHARED, dst_dma_fd, 0);
    if (dst_map == MAP_FAILED) {
        int saved_errno = errno;
        munmap(src_map, src_query.value().size);
        close(dst_dma_fd);
        close(src_dma_fd);
        close(fd);
        return Result<void, std::string>::error(
            "mmap destination dma-buf failed: " + std::string(strerror(saved_errno)));
    }

    std::memcpy(dst_map, src_map, bytes);

    munmap(dst_map, dst_query.value().size);
    munmap(src_map, src_query.value().size);
    close(dst_dma_fd);
    close(src_dma_fd);
    close(fd);

    SL_DEBUG("mesh: local VPU DMA-BUF transfer copied {} bytes from handle {} to handle {}",
             bytes, src_handle, dst_handle);
    return Result<void, std::string>::ok();
}

Result<void, std::string> GpuPool::remote_transfer(
    const MeshAllocation& src, const MeshAllocation& dst)
{
    (void)src.gpu_index;
    (void)dst.gpu_index;

    const std::string local_tmp = temp_path("straylight-mesh-copy");
    std::string src_remote_tmp;
    std::string dst_remote_tmp;

    auto cleanup = [&]() {
        unlink(local_tmp.c_str());
        if (!src_remote_tmp.empty()) {
            for (int attempt = 0; attempt < 3; ++attempt) {
                auto res = remote_exec(src.host, "rm -f " + shell_quote(src_remote_tmp));
                if (res.has_value()) break;
                if (attempt == 2) {
                    SL_WARN("mesh: remote cleanup failed on {} for {}: {}",
                            src.host, src_remote_tmp, res.error());
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                }
            }
        }
        if (!dst_remote_tmp.empty()) {
            for (int attempt = 0; attempt < 3; ++attempt) {
                auto res = remote_exec(dst.host, "rm -f " + shell_quote(dst_remote_tmp));
                if (res.has_value()) break;
                if (attempt == 2) {
                    SL_WARN("mesh: remote cleanup failed on {} for {}: {}",
                            dst.host, dst_remote_tmp, res.error());
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                }
            }
        }
    };

    auto fail = [&](const std::string& message) {
        cleanup();
        return Result<void, std::string>::error(message);
    };

    if (src.is_local) {
        auto local_dump = run_checked(
            std::string(VPU_REMOTE_HELPER) + " dump " +
            std::to_string(src.handle) + " " +
            std::to_string(src.size_bytes) + " " + shell_quote(local_tmp),
            "local VPU dump failed");
        if (!local_dump.has_value()) return fail(local_dump.error());
    } else {
        src_remote_tmp = temp_path("straylight-mesh-src");
        auto remote_dump = remote_exec(
            src.host, std::string(VPU_REMOTE_HELPER) + " dump " +
                          std::to_string(src.handle) + " " +
                          std::to_string(src.size_bytes) + " " +
                          shell_quote(src_remote_tmp));
        if (!remote_dump.has_value()) return fail(remote_dump.error());

        auto download = remote_download_file(src.host, src_remote_tmp, local_tmp);
        if (!download.has_value()) return fail(download.error());
    }

    if (dst.is_local) {
        auto local_load = run_checked(
            std::string(VPU_REMOTE_HELPER) + " load " +
            std::to_string(dst.handle) + " " + shell_quote(local_tmp),
            "local VPU load failed");
        if (!local_load.has_value()) return fail(local_load.error());
    } else {
        dst_remote_tmp = temp_path("straylight-mesh-dst");
        auto upload = remote_upload_file(dst.host, local_tmp, dst_remote_tmp);
        if (!upload.has_value()) return fail(upload.error());

        auto remote_load = remote_exec(
            dst.host, std::string(VPU_REMOTE_HELPER) + " load " +
                          std::to_string(dst.handle) + " " +
                          shell_quote(dst_remote_tmp));
        if (!remote_load.has_value()) return fail(remote_load.error());
    }

    cleanup();

    return Result<void, std::string>::ok();
}

Result<void, std::string> GpuPool::transfer(const MeshAllocation& src,
                                              const MeshAllocation& dst) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (src.size_bytes != dst.size_bytes) {
        return Result<void, std::string>::error(
            "Transfer size mismatch: src=" + std::to_string(src.size_bytes) +
            " dst=" + std::to_string(dst.size_bytes));
    }

    if (src.is_local && dst.is_local) {
        return local_to_local_transfer(src.gpu_index, src.handle,
                                        dst.gpu_index, dst.handle,
                                        src.size_bytes);
    }

    return remote_transfer(src, dst);
}

// ---------------------------------------------------------------------------
// Submit
// ---------------------------------------------------------------------------

Result<std::string, std::string> GpuPool::remote_exec(const std::string& host,
                                                        const std::string& command) {
    auto [output, ok] = run_command(
        std::string(REMOTE_CLI_PATH) + " exec --key " +
        shell_quote(remote_key_path()) + " " + shell_quote(host) + " " +
        shell_quote(command) + " 2>&1");

    if (!ok) {
        return Result<std::string, std::string>::error(
            "Remote exec failed on " + host + ": " + output);
    }

    return Result<std::string, std::string>::ok(output);
}

Result<std::string, std::string> GpuPool::submit(const std::string& command,
                                                   size_t vram_needed) {
    std::lock_guard<std::mutex> lock(mutex_);

    RemoteGpu* best = nullptr;
    for (auto& gpu : gpus_) {
        if (!gpu.is_available) continue;
        if (gpu.vram_available < vram_needed) continue;
        if (!best || gpu.utilization < best->utilization) {
            best = &gpu;
        }
    }

    if (!best) {
        return Result<std::string, std::string>::error(
            "No GPU available with " + std::to_string(vram_needed / (1024 * 1024)) +
            " MiB free VRAM");
    }

    SL_INFO("mesh: submitting '{}' to {}:gpu{} (util={:.1f}%, vram_free={}MiB)",
            command, best->host, best->gpu_index,
            best->utilization * 100.0f,
            best->vram_available / (1024 * 1024));

    if (best->is_local) {
        auto [output, ok] = run_command(
            "CUDA_VISIBLE_DEVICES=" + std::to_string(best->gpu_index) +
            " " + command + " 2>&1");

        if (!ok) {
            return Result<std::string, std::string>::error(
                "Local execution failed: " + output);
        }

        return Result<std::string, std::string>::ok(output);
    }

    return remote_exec(best->host, command);
}

// ---------------------------------------------------------------------------
// Maintenance
// ---------------------------------------------------------------------------

Result<void, std::string> GpuPool::refresh_stats() {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto cached_remote = read_cached_remote_stats();

    for (auto& gpu : gpus_) {
        if (gpu.is_local) {
            VpuStats stats;
            if (read_vpu_stats(stats)) {
                gpu.vram_total     = stats.total_bytes;
                gpu.vram_available = stats.free_bytes;
                gpu.temperature    = 0.0f;
                gpu.utilization    = stats.utilization;
                gpu.last_seen      = std::chrono::steady_clock::now();
            }
        } else {
            const auto cached = cached_remote.find(gpu.host);
            if (cached != cached_remote.end()) {
                gpu.vram_total     = cached->second.total_bytes;
                gpu.vram_available = cached->second.free_bytes;
                gpu.temperature    = cached->second.temperature;
                gpu.utilization    = cached->second.utilization;
                gpu.is_available   = true;
                gpu.last_seen      = std::chrono::steady_clock::now();
                continue;
            }

            auto info = remote_exec(
                gpu.host, std::string(VPU_REMOTE_HELPER) + " gpu-info " +
                              std::to_string(gpu.gpu_index));

            if (info.has_value() && !info.value().empty()) {
                std::istringstream iss(info.value());
                std::string name, vendor;
                size_t vtotal = 0, vfree = 0;
                float temp = 0, util = 0;
                if (iss >> name >> vendor >> vtotal >> vfree >> temp >> util) {
                    gpu.vram_total     = vtotal;
                    gpu.vram_available = vfree;
                    gpu.temperature    = temp;
                    gpu.utilization    = util;
                    gpu.is_available   = true;
                    gpu.last_seen      = std::chrono::steady_clock::now();
                }
            }
        }
    }

    return Result<void, std::string>::ok();
}

void GpuPool::mark_unavailable(const std::string& host, uint32_t gpu_index) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& gpu : gpus_) {
        if (gpu.host == host && gpu.gpu_index == gpu_index) {
            gpu.is_available = false;
            SL_WARN("mesh: marked {}:gpu{} as unavailable", host, gpu_index);
            break;
        }
    }
}

void GpuPool::mark_available(const std::string& host, uint32_t gpu_index) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& gpu : gpus_) {
        if (gpu.host == host && gpu.gpu_index == gpu_index) {
            gpu.is_available = true;
            SL_INFO("mesh: marked {}:gpu{} as available", host, gpu_index);
            break;
        }
    }
}

void GpuPool::remove_host(const std::string& host) {
    std::lock_guard<std::mutex> lock(mutex_);
    gpus_.erase(
        std::remove_if(gpus_.begin(), gpus_.end(),
            [&](const RemoteGpu& g) { return g.host == host; }),
        gpus_.end());
    SL_INFO("mesh: removed all GPUs from host {}", host);
}

} // namespace straylight
