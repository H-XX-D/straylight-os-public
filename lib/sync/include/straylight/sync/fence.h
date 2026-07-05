/**
 * StrayLight Sync Kernel — SyncFence
 *
 * Binary signal/wait primitive inspired by NVIDIA's sync fences.
 * Fences are shareable across processes via file descriptor passing
 * (SCM_RIGHTS over Unix sockets), enabling GPU-style cross-process
 * synchronization without shared memory.
 *
 * States:  Unsignaled → Signaled  (one-shot, non-resettable)
 *
 * Backed by Linux eventfd(2) so the kernel handles the wake-up
 * efficiently — no busy spinning, no userspace futex overhead.
 */
#pragma once

#include <straylight/result.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace straylight::sync {

/// Fence state — matches NVIDIA's binary fence model.
enum class FenceState : uint8_t {
    Unsignaled = 0,
    Signaled   = 1,
    Error      = 2
};

inline const char* fence_state_str(FenceState s) {
    switch (s) {
        case FenceState::Unsignaled: return "unsignaled";
        case FenceState::Signaled:   return "signaled";
        case FenceState::Error:      return "error";
    }
    return "unknown";
}

/// Unique fence identifier — globally unique within a sync domain.
struct FenceId {
    uint64_t domain;    // Sync domain (e.g., compositor, boot, IPC)
    uint64_t sequence;  // Monotonic within domain

    bool operator==(const FenceId& o) const {
        return domain == o.domain && sequence == o.sequence;
    }
    bool operator<(const FenceId& o) const {
        return domain < o.domain || (domain == o.domain && sequence < o.sequence);
    }
};

/// Fence creation options.
struct FenceCreateInfo {
    uint64_t domain = 0;
    std::string name;           // Human-readable label (debug)
    bool shared = false;        // If true, fd can be passed to other processes
    int timeout_ms = -1;        // Default wait timeout (-1 = infinite)
};

/// A sync fence — the fundamental sync primitive.
///
/// Under the hood this wraps a Linux eventfd. When signaled,
/// a uint64_t(1) is written to the eventfd, waking all waiters.
/// The eventfd file descriptor can be passed across processes
/// via Unix socket SCM_RIGHTS — exactly like NVIDIA passes
/// sync fences between GPU driver and compositor.
class SyncFence {
public:
    SyncFence();
    ~SyncFence();

    // Non-copyable, movable
    SyncFence(const SyncFence&) = delete;
    SyncFence& operator=(const SyncFence&) = delete;
    SyncFence(SyncFence&& o) noexcept;
    SyncFence& operator=(SyncFence&& o) noexcept;

    /// Create a new fence with the given options.
    static Result<SyncFence, std::string> create(const FenceCreateInfo& info);

    /// Import a fence from an existing file descriptor (received via SCM_RIGHTS).
    static Result<SyncFence, std::string> import_fd(int fd, const FenceId& id);

    /// Signal this fence — wakes all waiters. One-shot.
    VoidResult<std::string> signal();

    /// Wait for this fence to become signaled.
    /// @param timeout_ms  -1 = infinite, 0 = poll (non-blocking)
    VoidResult<std::string> wait(int timeout_ms = -1);

    /// Check state without blocking.
    [[nodiscard]] FenceState state() const { return state_.load(std::memory_order_acquire); }

    /// Get the underlying fd for passing to another process.
    [[nodiscard]] int fd() const { return event_fd_; }

    /// Get the fence ID.
    [[nodiscard]] const FenceId& id() const { return id_; }

    /// Get the human-readable name.
    [[nodiscard]] const std::string& name() const { return name_; }

    /// Wait on multiple fences — returns when ALL are signaled.
    /// Like NVIDIA's multi-fence sync or Vulkan's vkWaitForFences.
    static VoidResult<std::string> wait_all(const std::vector<SyncFence*>& fences,
                                              int timeout_ms = -1);

    /// Wait on multiple fences — returns index of FIRST signaled.
    /// Like select()/epoll() but for sync fences.
    static Result<size_t, std::string> wait_any(const std::vector<SyncFence*>& fences,
                                                  int timeout_ms = -1);

    /// Register a callback to fire when fence is signaled.
    /// Callback runs on an internal notification thread.
    void on_signal(std::function<void(const FenceId&)> callback);

private:
    int event_fd_ = -1;
    FenceId id_{};
    std::string name_;
    std::atomic<FenceState> state_{FenceState::Unsignaled};
    std::function<void(const FenceId&)> callback_;
};

} // namespace straylight::sync
