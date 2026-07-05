/**
 * StrayLight Fuse — Shared Memory Region Management
 *
 * Creates POSIX shared memory regions and manages their lifecycle.
 * Supports mapping into remote processes via /proc/PID/mem and
 * reference counting for automatic cleanup.
 */
#pragma once

#include "straylight/result.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace straylight::fuse {

/** Describes a single shared memory region. */
struct SharedRegion {
    std::string region_id;
    std::string shm_name;         // POSIX shm name (/straylight_fuse_<id>)
    size_t      size{0};
    void*       local_addr{nullptr};
    int         shm_fd{-1};
    int         flags{0};         // MAP_SHARED flags used
    std::atomic<int> ref_count{0};
    std::chrono::steady_clock::time_point created_at;
};

/** Per-process mapping record. */
struct ProcessMapping {
    pid_t       pid;
    std::string region_id;
    uint64_t    remote_addr{0};  // Address in target process
    size_t      size{0};
};

class SharedRegionManager {
public:
    SharedRegionManager() = default;
    ~SharedRegionManager() { cleanup_all(); }

    /**
     * Create a new shared memory region.
     * Returns the region_id on success.
     */
    Result<std::string, std::string> create_region(
            size_t size, int flags = 0) {
        std::lock_guard<std::mutex> lock(mu_);

        static uint64_t counter = 0;
        std::string region_id = "rgn-" + std::to_string(++counter);
        std::string shm_name = "/straylight_fuse_" + region_id;

        // Create POSIX shared memory object
        int shm_fd = shm_open(shm_name.c_str(),
                               O_CREAT | O_RDWR | O_EXCL, 0600);
        if (shm_fd < 0) {
            return Result<std::string, std::string>::error(
                "shm_open failed: " + std::string(strerror(errno)));
        }

        // Size the region
        if (ftruncate(shm_fd, static_cast<off_t>(size)) < 0) {
            close(shm_fd);
            shm_unlink(shm_name.c_str());
            return Result<std::string, std::string>::error(
                "ftruncate failed: " + std::string(strerror(errno)));
        }

        // Map into our address space
        int mmap_flags = MAP_SHARED;
        if (flags & MAP_POPULATE) mmap_flags |= MAP_POPULATE;

        void* addr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                          mmap_flags, shm_fd, 0);
        if (addr == MAP_FAILED) {
            close(shm_fd);
            shm_unlink(shm_name.c_str());
            return Result<std::string, std::string>::error(
                "mmap failed: " + std::string(strerror(errno)));
        }

        // Zero-initialize
        memset(addr, 0, size);

        auto region = std::make_unique<SharedRegion>();
        region->region_id = region_id;
        region->shm_name = shm_name;
        region->size = size;
        region->local_addr = addr;
        region->shm_fd = shm_fd;
        region->flags = mmap_flags;
        region->ref_count.store(1);
        region->created_at = std::chrono::steady_clock::now();

        regions_[region_id] = std::move(region);
        return Result<std::string, std::string>::ok(region_id);
    }

    /**
     * Map an existing region into a target process.
     *
     * On Linux this uses /proc/PID/mem to write the shm fd info,
     * and the target process picks it up via a pre-injected stub.
     * For cooperative processes, we write mapping instructions to a
     * well-known location that the fuse client library reads.
     */
    Result<ProcessMapping, std::string> map_into_process(
            pid_t pid, const std::string& region_id) {
        std::lock_guard<std::mutex> lock(mu_);

        auto it = regions_.find(region_id);
        if (it == regions_.end()) {
            return Result<ProcessMapping, std::string>::error(
                "region not found: " + region_id);
        }

        auto& region = it->second;

        // Verify target process exists
        std::string proc_path = "/proc/" + std::to_string(pid);
        struct stat st{};
        if (stat(proc_path.c_str(), &st) != 0) {
            return Result<ProcessMapping, std::string>::error(
                "process " + std::to_string(pid) + " not found");
        }

        // Read target process memory maps to find a suitable address
        std::string maps_path = proc_path + "/maps";
        uint64_t target_addr = find_free_region(maps_path, region->size);
        if (target_addr == 0) {
            return Result<ProcessMapping, std::string>::error(
                "no suitable address space in target process");
        }

        // Write mapping descriptor to the fuse spool for the target process.
        // The straylight-fuse client library in the target process polls this.
        std::string spool_dir = "/var/lib/straylight/fuse/mappings/" +
                                std::to_string(pid);
        {
            std::error_code ec;
            std::filesystem::create_directories(spool_dir, ec);
        }

        std::string desc_path = spool_dir + "/" + region_id + ".map";
        {
            std::ofstream desc(desc_path, std::ios::trunc);
            if (!desc) {
                return Result<ProcessMapping, std::string>::error(
                    "cannot write mapping descriptor");
            }
            desc << "shm_name=" << region->shm_name << "\n"
                 << "size=" << region->size << "\n"
                 << "target_addr=0x" << std::hex << target_addr << "\n"
                 << "flags=" << std::dec << region->flags << "\n";
        }

        // Use process_vm_writev to inject a small signal into the target
        // process's futex word (if it has the fuse client library loaded).
        // This is a cooperative protocol — the target must opt in.
        signal_process(pid);

        region->ref_count.fetch_add(1, std::memory_order_relaxed);

        ProcessMapping mapping{};
        mapping.pid = pid;
        mapping.region_id = region_id;
        mapping.remote_addr = target_addr;
        mapping.size = region->size;

        mappings_.push_back(mapping);

        return Result<ProcessMapping, std::string>::ok(mapping);
    }

    /**
     * Release a region. Decrements refcount; destroys if zero.
     */
    VoidResult<> release_region(const std::string& region_id) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = regions_.find(region_id);
        if (it == regions_.end()) {
            return VoidResult<>::error("region not found: " + region_id);
        }
        int prev = it->second->ref_count.fetch_sub(1, std::memory_order_acq_rel);
        if (prev <= 1) {
            destroy_region_locked(it->second.get());
            regions_.erase(it);
        }
        return VoidResult<>::ok();
    }

    /** Get info about a region. */
    Result<SharedRegion*, std::string> get_region(const std::string& region_id) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = regions_.find(region_id);
        if (it == regions_.end()) {
            return Result<SharedRegion*, std::string>::error(
                "region not found: " + region_id);
        }
        return Result<SharedRegion*, std::string>::ok(it->second.get());
    }

    /** List all mappings for a given process. */
    std::vector<ProcessMapping> get_process_mappings(pid_t pid) const {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<ProcessMapping> result;
        for (const auto& m : mappings_) {
            if (m.pid == pid) result.push_back(m);
        }
        return result;
    }

    /** Number of active regions. */
    size_t active_region_count() const {
        std::lock_guard<std::mutex> lock(mu_);
        return regions_.size();
    }

    /** Total mapped bytes across all regions. */
    size_t total_mapped_bytes() const {
        std::lock_guard<std::mutex> lock(mu_);
        size_t total = 0;
        for (const auto& [_, r] : regions_) total += r->size;
        return total;
    }

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::unique_ptr<SharedRegion>> regions_;
    std::vector<ProcessMapping> mappings_;

    void destroy_region_locked(SharedRegion* r) {
        if (r->local_addr && r->local_addr != MAP_FAILED) {
            munmap(r->local_addr, r->size);
            r->local_addr = nullptr;
        }
        if (r->shm_fd >= 0) {
            close(r->shm_fd);
            r->shm_fd = -1;
        }
        shm_unlink(r->shm_name.c_str());

        // Remove mapping descriptors
        auto mit = mappings_.begin();
        while (mit != mappings_.end()) {
            if (mit->region_id == r->region_id) {
                std::string desc = "/var/lib/straylight/fuse/mappings/" +
                    std::to_string(mit->pid) + "/" + r->region_id + ".map";
                unlink(desc.c_str());
                mit = mappings_.erase(mit);
            } else {
                ++mit;
            }
        }
    }

    void cleanup_all() {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& [_, r] : regions_) {
            destroy_region_locked(r.get());
        }
        regions_.clear();
    }

    /** Parse /proc/PID/maps to find a gap large enough for `size` bytes. */
    uint64_t find_free_region(const std::string& maps_path, size_t size) {
        std::ifstream maps(maps_path);
        if (!maps) return 0;

        uint64_t prev_end = 0x100000000ULL; // Start searching above 4 GiB
        std::string line;
        while (std::getline(maps, line)) {
            uint64_t start = 0, end = 0;
            if (sscanf(line.c_str(), "%lx-%lx", &start, &end) == 2) {
                if (start > prev_end && (start - prev_end) >= size) {
                    // Align to page boundary
                    uint64_t aligned = (prev_end + 0xFFF) & ~0xFFFULL;
                    if (aligned + size <= start) {
                        return aligned;
                    }
                }
                prev_end = end;
            }
        }

        // Try after the last mapping
        uint64_t aligned = (prev_end + 0xFFF) & ~0xFFFULL;
        return aligned;
    }

    /** Signal a process that a new mapping is available. */
    void signal_process(pid_t pid) {
        // Send SIGUSR1 to notify the fuse client library
        kill(pid, SIGUSR1);
    }
};

} // namespace straylight::fuse
