/**
 * StrayLight Bridge Page Tracker — Dirty page tracking for shared memory.
 *
 * Uses /proc/PID/pagemap + soft-dirty bits or userfaultfd to detect
 * which pages within a shared memory region have been modified since
 * the last sync.
 */
#pragma once

#include "straylight/result.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace straylight::bridge {

/// Information about a dirty page.
struct DirtyPage {
    uint64_t offset;        // Offset within the shared region
    size_t size;            // Page size (typically 4096)
    uint64_t sequence;      // Monotonic sequence number for ordering
};

/// Page tracking mode.
enum class TrackingMode {
    SoftDirty,      // Uses /proc/PID/pagemap soft-dirty bits
    Userfaultfd,    // Uses userfaultfd to trap writes
    Manual          // Application explicitly marks pages dirty
};

inline const char* tracking_mode_to_string(TrackingMode m) {
    switch (m) {
        case TrackingMode::SoftDirty:   return "soft_dirty";
        case TrackingMode::Userfaultfd: return "userfaultfd";
        case TrackingMode::Manual:      return "manual";
    }
    return "unknown";
}

class PageTracker {
public:
    PageTracker();
    ~PageTracker();

    /// Initialize tracking for a memory region.
    VoidResult<std::string> init(void* base_addr, size_t region_size,
                                  TrackingMode mode = TrackingMode::SoftDirty);

    /// Get list of dirty pages since last clear.
    Result<std::vector<DirtyPage>, std::string> get_dirty_pages();

    /// Clear dirty flags after sync.
    VoidResult<std::string> clear_dirty_flags();

    /// Manually mark a page as dirty (for Manual mode).
    void mark_dirty(uint64_t offset, size_t size);

    /// Get total number of pages being tracked.
    [[nodiscard]] size_t total_pages() const { return total_pages_; }

    /// Get number of currently dirty pages.
    [[nodiscard]] size_t dirty_count() const;

    /// Get tracking statistics.
    struct Stats {
        uint64_t total_syncs;
        uint64_t total_dirty_pages;
        uint64_t total_bytes_tracked;
        double avg_dirty_ratio;     // Average fraction of pages dirty per sync
    };
    Stats get_stats() const;

    /// Cleanup.
    void shutdown();

private:
    void* base_addr_ = nullptr;
    size_t region_size_ = 0;
    size_t page_size_ = 4096;
    size_t total_pages_ = 0;
    TrackingMode mode_ = TrackingMode::SoftDirty;
    uint64_t sequence_ = 0;

    // Soft-dirty tracking state.
    int pagemap_fd_ = -1;
    pid_t tracked_pid_ = 0;

    // Userfaultfd tracking state.
    int uffd_ = -1;

    // Manual tracking state.
    std::vector<bool> dirty_bitmap_;
    mutable std::mutex bitmap_mutex_;

    // Statistics.
    mutable std::mutex stats_mutex_;
    uint64_t total_syncs_ = 0;
    uint64_t total_dirty_pages_ = 0;

    /// Read soft-dirty bits from pagemap.
    Result<std::vector<DirtyPage>, std::string> get_dirty_softdirty();

    /// Read dirty pages from userfaultfd.
    Result<std::vector<DirtyPage>, std::string> get_dirty_userfaultfd();

    /// Read dirty pages from manual bitmap.
    Result<std::vector<DirtyPage>, std::string> get_dirty_manual();

    /// Clear soft-dirty bits via /proc/PID/clear_refs.
    VoidResult<std::string> clear_softdirty();
};

} // namespace straylight::bridge
