/**
 * StrayLight Sync Kernel — TimelineSemaphore
 *
 * Monotonically increasing counter where waiters block until
 * value >= target. Direct analog of NVIDIA's timeline semaphores
 * and Vulkan's VkSemaphore (timeline type).
 *
 * Use cases:
 *   - Frame pacing: compositor waits for GPU fence N, then presents
 *   - Service ordering: boot manager signals stage N, services wait
 *   - Pipeline stages: each stage waits for (N-1), signals N
 *
 * Backed by a combination of eventfd + futex for efficient
 * single-process and cross-process wake-up.
 */
#pragma once

#include <straylight/result.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace straylight::sync {

/// Timeline creation options.
struct TimelineCreateInfo {
    std::string name;
    uint64_t initial_value = 0;     // Starting counter value
    uint64_t max_value = UINT64_MAX; // Optional ceiling
    bool shared = false;             // Cross-process capable
};

/// A point on the timeline — used for registering waits.
struct TimelinePoint {
    uint64_t value;
    std::string label;  // Optional debug label (e.g., "frame-42")
};

/// Statistics for a timeline.
struct TimelineStats {
    uint64_t current_value;
    uint64_t signal_count;       // How many times signal() was called
    uint64_t wait_count;         // How many wait() calls completed
    uint64_t timeout_count;      // How many waits timed out
    double avg_wait_us;          // Average wait duration in microseconds
    double max_wait_us;
};

/// A timeline semaphore — monotonic counter with efficient waits.
///
/// The NVIDIA GPU driver uses timeline semaphores to order work
/// across command buffers. StrayLight uses the same concept to
/// order work across services and compositor frames.
///
/// Example (boot ordering):
///   timeline.signal(1);  // kernel modules loaded
///   timeline.signal(2);  // networking up
///   timeline.signal(3);  // desktop ready
///   // Any service can: timeline.wait(2) to block until networking
class TimelineSemaphore {
public:
    TimelineSemaphore();
    ~TimelineSemaphore();

    // Non-copyable, movable
    TimelineSemaphore(const TimelineSemaphore&) = delete;
    TimelineSemaphore& operator=(const TimelineSemaphore&) = delete;
    TimelineSemaphore(TimelineSemaphore&& o) noexcept;
    TimelineSemaphore& operator=(TimelineSemaphore&& o) noexcept;

    /// Create a new timeline semaphore.
    static Result<TimelineSemaphore, std::string> create(const TimelineCreateInfo& info);

    /// Import from fd (for cross-process sharing).
    static Result<TimelineSemaphore, std::string> import_fd(int fd, const std::string& name);

    /// Advance the timeline to the given value. Must be > current.
    /// Wakes all waiters whose target <= new_value.
    VoidResult<std::string> signal(uint64_t value);

    /// Convenience: advance by 1 and return new value.
    Result<uint64_t, std::string> increment();

    /// Block until timeline value >= target.
    VoidResult<std::string> wait(uint64_t target, int timeout_ms = -1);

    /// Non-blocking check: is timeline >= target?
    [[nodiscard]] bool reached(uint64_t target) const {
        return value_.load(std::memory_order_acquire) >= target;
    }

    /// Current timeline value.
    [[nodiscard]] uint64_t value() const {
        return value_.load(std::memory_order_acquire);
    }

    /// Get the underlying fd for cross-process sharing.
    [[nodiscard]] int fd() const { return event_fd_; }

    /// Get name.
    [[nodiscard]] const std::string& name() const { return name_; }

    /// Register callback for when timeline reaches a specific point.
    void on_reach(uint64_t target, std::function<void(uint64_t)> callback);

    /// Get statistics.
    TimelineStats stats() const;

private:
    int event_fd_ = -1;
    std::string name_;
    std::atomic<uint64_t> value_{0};
    uint64_t max_value_ = UINT64_MAX;

    // Waiter tracking
    struct Waiter {
        uint64_t target;
        std::function<void(uint64_t)> callback;
    };
    mutable std::mutex waiters_mutex_;
    std::vector<Waiter> waiters_;

    // Stats
    mutable std::mutex stats_mutex_;
    uint64_t signal_count_ = 0;
    uint64_t wait_count_ = 0;
    uint64_t timeout_count_ = 0;
    double total_wait_us_ = 0.0;
    double max_wait_us_ = 0.0;
};

} // namespace straylight::sync
