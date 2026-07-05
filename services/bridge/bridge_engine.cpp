/**
 * StrayLight Bridge Engine — Implementation.
 */

#include "bridge_engine.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace straylight::bridge {

BridgeEngine::BridgeEngine() = default;

BridgeEngine::~BridgeEngine() {
    stop_sync_thread();

    // Destroy all bridges.
    std::lock_guard<std::mutex> lock(bridges_mutex_);
    for (auto& [id, state] : bridges_) {
        if (state->descriptor.local_addr) {
            munmap(state->descriptor.local_addr, state->descriptor.size);
        }
        if (state->descriptor.shm_fd >= 0) {
            ::close(state->descriptor.shm_fd);
            shm_unlink(state->descriptor.region_name.c_str());
        }
        state->sync.close();
        state->tracker.shutdown();
    }
}

// ---------------------------------------------------------------------------
// Bridge creation / destruction
// ---------------------------------------------------------------------------

Result<BridgeId, std::string> BridgeEngine::create_bridge(
    const std::string& remote_host,
    const std::string& region_name,
    size_t size,
    SyncMode mode,
    bool encrypted)
{
    std::lock_guard<std::mutex> lock(bridges_mutex_);

    // Check for duplicate region name.
    for (const auto& [id, state] : bridges_) {
        if (state->descriptor.region_name == region_name) {
            return Result<BridgeId, std::string>::error(
                "bridge with name '" + region_name + "' already exists");
        }
    }

    auto bridge_state = std::make_unique<BridgeState>();
    BridgeId id = next_id_++;

    // Create shared memory.
    std::string shm_name = "/" + region_name;
    int shm_fd = shm_open(shm_name.c_str(), O_CREAT | O_RDWR, 0666);
    if (shm_fd < 0) {
        return Result<BridgeId, std::string>::error(
            "shm_open failed: " + std::string(strerror(errno)));
    }

    if (ftruncate(shm_fd, static_cast<off_t>(size)) < 0) {
        ::close(shm_fd);
        shm_unlink(shm_name.c_str());
        return Result<BridgeId, std::string>::error(
            "ftruncate failed: " + std::string(strerror(errno)));
    }

    void* addr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (addr == MAP_FAILED) {
        ::close(shm_fd);
        shm_unlink(shm_name.c_str());
        return Result<BridgeId, std::string>::error(
            "mmap failed: " + std::string(strerror(errno)));
    }

    // Zero-initialize.
    std::memset(addr, 0, size);

    // Initialize page tracker.
    auto tracker_result = bridge_state->tracker.init(addr, size, TrackingMode::Manual);
    if (!tracker_result) {
        munmap(addr, size);
        ::close(shm_fd);
        shm_unlink(shm_name.c_str());
        return Result<BridgeId, std::string>::error(
            "page tracker init failed: " + tracker_result.err());
    }

    // Connect to remote bridge daemon.
    uint16_t port = default_port_;
    auto connect_result = bridge_state->sync.connect(remote_host, port);
    if (!connect_result) {
        fprintf(stderr, "[bridge] warning: cannot connect to %s:%d — %s\n",
                remote_host.c_str(), port, connect_result.err().c_str());
        fprintf(stderr, "[bridge] bridge created in disconnected mode, will retry on sync\n");
    }

    bridge_state->sync.set_sync_mode(mode);
    if (encrypted) {
        bridge_state->sync.enable_encryption(true);
    }

    // Fill descriptor.
    bridge_state->descriptor.id = id;
    bridge_state->descriptor.region_name = region_name;
    bridge_state->descriptor.remote_host = remote_host;
    bridge_state->descriptor.remote_port = port;
    bridge_state->descriptor.size = size;
    bridge_state->descriptor.sync_mode = mode;
    bridge_state->descriptor.encrypted = encrypted;
    bridge_state->descriptor.local_addr = addr;
    bridge_state->descriptor.shm_fd = shm_fd;
    bridge_state->descriptor.created_at = std::chrono::steady_clock::now();

    fprintf(stdout, "[bridge] created bridge %u: %s -> %s:%d (%zu bytes, mode=%s)\n",
            id, region_name.c_str(), remote_host.c_str(), port,
            size, sync_mode_to_string(mode));

    bridges_[id] = std::move(bridge_state);
    return Result<BridgeId, std::string>::ok(id);
}

VoidResult<std::string> BridgeEngine::destroy_bridge(BridgeId id) {
    std::lock_guard<std::mutex> lock(bridges_mutex_);

    auto it = bridges_.find(id);
    if (it == bridges_.end()) {
        return VoidResult<std::string>::error("bridge not found: " + std::to_string(id));
    }

    auto& state = it->second;

    // Stop sync thread if running.
    state->running.store(false);
    if (state->sync_thread.joinable()) {
        state->sync_thread.join();
    }

    // Cleanup shared memory.
    if (state->descriptor.local_addr) {
        munmap(state->descriptor.local_addr, state->descriptor.size);
    }
    if (state->descriptor.shm_fd >= 0) {
        ::close(state->descriptor.shm_fd);
        std::string shm_name = "/" + state->descriptor.region_name;
        shm_unlink(shm_name.c_str());
    }

    // Close network connection.
    state->sync.close();
    state->tracker.shutdown();

    fprintf(stdout, "[bridge] destroyed bridge %u: %s\n",
            id, state->descriptor.region_name.c_str());

    bridges_.erase(it);
    return VoidResult<std::string>::ok();
}

// ---------------------------------------------------------------------------
// Sync
// ---------------------------------------------------------------------------

VoidResult<std::string> BridgeEngine::sync_bridge(BridgeId id) {
    std::lock_guard<std::mutex> lock(bridges_mutex_);

    auto it = bridges_.find(id);
    if (it == bridges_.end()) {
        return VoidResult<std::string>::error("bridge not found: " + std::to_string(id));
    }

    return do_sync(*it->second);
}

VoidResult<std::string> BridgeEngine::do_sync(BridgeState& state) {
    if (!state.sync.is_connected()) {
        // Try to reconnect.
        auto conn = state.sync.connect(state.descriptor.remote_host,
                                         state.descriptor.remote_port);
        if (!conn) {
            return VoidResult<std::string>::error("not connected: " + conn.err());
        }
    }

    // Get dirty pages.
    auto dirty_result = state.tracker.get_dirty_pages();
    if (!dirty_result) {
        return VoidResult<std::string>::error("get_dirty_pages: " + dirty_result.err());
    }

    auto& dirty_pages = dirty_result.value();
    if (dirty_pages.empty()) {
        return VoidResult<std::string>::ok(); // Nothing to sync.
    }

    // Sync pages to remote.
    auto sync_result = state.sync.sync_pages(
        state.descriptor.id,
        dirty_pages,
        state.descriptor.local_addr);

    if (!sync_result) {
        return VoidResult<std::string>::error("sync_pages: " + sync_result.err());
    }

    // Clear dirty flags.
    auto clear_result = state.tracker.clear_dirty_flags();
    if (!clear_result) {
        fprintf(stderr, "[bridge] warning: clear_dirty_flags failed: %s\n",
                clear_result.err().c_str());
    }

    // Update descriptor stats.
    state.descriptor.total_syncs++;
    for (const auto& p : dirty_pages) {
        state.descriptor.total_bytes_synced += p.size;
    }

    return VoidResult<std::string>::ok();
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

Result<void*, std::string> BridgeEngine::get_local_addr(BridgeId id) {
    std::lock_guard<std::mutex> lock(bridges_mutex_);

    auto it = bridges_.find(id);
    if (it == bridges_.end()) {
        return Result<void*, std::string>::error("bridge not found");
    }

    return Result<void*, std::string>::ok(it->second->descriptor.local_addr);
}

std::vector<BridgeDescriptor> BridgeEngine::list_bridges() const {
    std::lock_guard<std::mutex> lock(bridges_mutex_);
    std::vector<BridgeDescriptor> result;
    for (const auto& [id, state] : bridges_) {
        result.push_back(state->descriptor);
    }
    return result;
}

Result<BridgeStats, std::string> BridgeEngine::get_stats(BridgeId id) const {
    std::lock_guard<std::mutex> lock(bridges_mutex_);

    auto it = bridges_.find(id);
    if (it == bridges_.end()) {
        return Result<BridgeStats, std::string>::error("bridge not found");
    }

    const auto& state = *it->second;
    auto sync_stats = state.sync.get_stats();

    BridgeStats stats;
    stats.id = id;
    stats.region_name = state.descriptor.region_name;
    stats.region_size = state.descriptor.size;
    stats.total_syncs = state.descriptor.total_syncs;
    stats.total_pages_synced = sync_stats.total_pages_sent;
    stats.total_bytes_synced = state.descriptor.total_bytes_synced;
    stats.dirty_pages_current = state.tracker.dirty_count();
    stats.avg_sync_latency_ms = sync_stats.avg_sync_latency_ms;
    stats.sync_mode = state.descriptor.sync_mode;
    stats.connected = state.sync.is_connected();

    auto now = std::chrono::steady_clock::now();
    stats.uptime_seconds = std::chrono::duration<double>(
        now - state.descriptor.created_at).count();

    return Result<BridgeStats, std::string>::ok(stats);
}

// ---------------------------------------------------------------------------
// Background sync thread
// ---------------------------------------------------------------------------

void BridgeEngine::start_sync_thread() {
    if (batch_running_.load()) return;

    batch_running_.store(true);
    batch_thread_ = std::thread([this]() { batch_sync_loop(); });
}

void BridgeEngine::stop_sync_thread() {
    batch_running_.store(false);
    if (batch_thread_.joinable()) {
        batch_thread_.join();
    }
}

void BridgeEngine::batch_sync_loop() {
    fprintf(stdout, "[bridge] batch sync thread started\n");

    while (batch_running_.load()) {
        {
            std::lock_guard<std::mutex> lock(bridges_mutex_);

            for (auto& [id, state] : bridges_) {
                if (!batch_running_.load()) break;

                SyncMode mode = state->descriptor.sync_mode;
                if (mode == SyncMode::Manual) continue;

                // For immediate mode, sync every iteration.
                // For batched mode, sync at configured interval.
                auto result = do_sync(*state);
                if (!result) {
                    // Non-fatal: log and continue.
                    fprintf(stderr, "[bridge] sync error on bridge %u: %s\n",
                            id, result.err().c_str());
                }
            }
        }

        // Sleep based on the fastest bridge interval.
        // Default 10ms for immediate, 100ms for batched.
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    fprintf(stdout, "[bridge] batch sync thread stopped\n");
}

} // namespace straylight::bridge
