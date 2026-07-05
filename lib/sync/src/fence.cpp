/**
 * StrayLight Sync Kernel — SyncFence implementation
 *
 * Uses Linux eventfd(2) as the backing primitive. This gives us:
 *   - Kernel-managed wake-up (no busy spinning)
 *   - poll()/epoll() compatible (integrates with event loops)
 *   - File descriptor passable via SCM_RIGHTS (cross-process)
 *   - Lightweight: one fd per fence, no shared memory needed
 */
#include "straylight/sync/fence.h"

#include <sys/eventfd.h>
#include <poll.h>
#include <unistd.h>
#include <errno.h>
#include <cstring>
#include <algorithm>

namespace straylight::sync {

static std::atomic<uint64_t> s_fence_seq{1};

// ── Construction / Destruction ──────────────────────────────────

SyncFence::SyncFence() = default;

SyncFence::~SyncFence() {
    if (event_fd_ >= 0) {
        ::close(event_fd_);
    }
}

SyncFence::SyncFence(SyncFence&& o) noexcept
    : event_fd_(o.event_fd_)
    , id_(o.id_)
    , name_(std::move(o.name_))
    , state_(o.state_.load())
    , callback_(std::move(o.callback_))
{
    o.event_fd_ = -1;
    o.state_.store(FenceState::Error);
}

SyncFence& SyncFence::operator=(SyncFence&& o) noexcept {
    if (this != &o) {
        if (event_fd_ >= 0) ::close(event_fd_);
        event_fd_ = o.event_fd_;
        id_ = o.id_;
        name_ = std::move(o.name_);
        state_.store(o.state_.load());
        callback_ = std::move(o.callback_);
        o.event_fd_ = -1;
        o.state_.store(FenceState::Error);
    }
    return *this;
}

// ── Factory ─────────────────────────────────────────────────────

Result<SyncFence, std::string> SyncFence::create(const FenceCreateInfo& info) {
    // EFD_SEMAPHORE makes it behave like a semaphore (read decrements by 1).
    // For a binary fence we just use plain eventfd.
    int flags = EFD_NONBLOCK | EFD_CLOEXEC;
    int fd = ::eventfd(0, flags);
    if (fd < 0) {
        return Result<SyncFence, std::string>::error(
            std::string("eventfd() failed: ") + ::strerror(errno));
    }

    SyncFence fence;
    fence.event_fd_ = fd;
    fence.id_ = FenceId{info.domain, s_fence_seq.fetch_add(1)};
    fence.name_ = info.name.empty()
        ? ("fence-" + std::to_string(fence.id_.sequence))
        : info.name;
    fence.state_.store(FenceState::Unsignaled);

    return Result<SyncFence, std::string>::ok(std::move(fence));
}

Result<SyncFence, std::string> SyncFence::import_fd(int fd, const FenceId& id) {
    if (fd < 0) {
        return Result<SyncFence, std::string>::error("Invalid fd for import");
    }

    SyncFence fence;
    fence.event_fd_ = fd;
    fence.id_ = id;
    fence.name_ = "imported-" + std::to_string(id.sequence);
    fence.state_.store(FenceState::Unsignaled);

    return Result<SyncFence, std::string>::ok(std::move(fence));
}

// ── Signal ──────────────────────────────────────────────────────

VoidResult<std::string> SyncFence::signal() {
    auto expected = FenceState::Unsignaled;
    if (!state_.compare_exchange_strong(expected, FenceState::Signaled)) {
        return VoidResult<std::string>::error("Fence already signaled or in error state");
    }

    uint64_t val = 1;
    ssize_t n = ::write(event_fd_, &val, sizeof(val));
    if (n != sizeof(val)) {
        state_.store(FenceState::Error);
        return VoidResult<std::string>::error(
            std::string("eventfd write failed: ") + ::strerror(errno));
    }

    // Fire callback if registered
    if (callback_) {
        callback_(id_);
    }

    return VoidResult<std::string>::ok();
}

// ── Wait ────────────────────────────────────────────────────────

VoidResult<std::string> SyncFence::wait(int timeout_ms) {
    // Fast path: already signaled
    if (state_.load(std::memory_order_acquire) == FenceState::Signaled) {
        return VoidResult<std::string>::ok();
    }

    if (state_.load(std::memory_order_acquire) == FenceState::Error) {
        return VoidResult<std::string>::error("Fence is in error state");
    }

    struct pollfd pfd{};
    pfd.fd = event_fd_;
    pfd.events = POLLIN;

    int rc = ::poll(&pfd, 1, timeout_ms);
    if (rc < 0) {
        return VoidResult<std::string>::error(
            std::string("poll() failed: ") + ::strerror(errno));
    }
    if (rc == 0) {
        return VoidResult<std::string>::error("Fence wait timed out");
    }

    // Drain the eventfd
    uint64_t val = 0;
    ::read(event_fd_, &val, sizeof(val));

    return VoidResult<std::string>::ok();
}

// ── Multi-fence operations ──────────────────────────────────────

VoidResult<std::string> SyncFence::wait_all(
    const std::vector<SyncFence*>& fences, int timeout_ms)
{
    if (fences.empty()) return VoidResult<std::string>::ok();

    // Use poll on all fds simultaneously
    std::vector<struct pollfd> pfds(fences.size());
    std::vector<bool> done(fences.size(), false);
    size_t remaining = fences.size();

    for (size_t i = 0; i < fences.size(); ++i) {
        // Fast path for already-signaled fences
        if (fences[i]->state() == FenceState::Signaled) {
            done[i] = true;
            remaining--;
            pfds[i].fd = -1;  // Skip in poll
        } else {
            pfds[i].fd = fences[i]->fd();
        }
        pfds[i].events = POLLIN;
    }

    auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(timeout_ms < 0 ? INT32_MAX : timeout_ms);

    while (remaining > 0) {
        int time_left = -1;
        if (timeout_ms >= 0) {
            auto now = std::chrono::steady_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            if (ms <= 0) return VoidResult<std::string>::error("wait_all timed out");
            time_left = static_cast<int>(ms);
        }

        int rc = ::poll(pfds.data(), pfds.size(), time_left);
        if (rc < 0 && errno != EINTR) {
            return VoidResult<std::string>::error(
                std::string("poll() failed: ") + ::strerror(errno));
        }

        for (size_t i = 0; i < fences.size(); ++i) {
            if (!done[i] && (pfds[i].revents & POLLIN)) {
                uint64_t val = 0;
                ::read(pfds[i].fd, &val, sizeof(val));
                done[i] = true;
                pfds[i].fd = -1;
                remaining--;
            }
        }
    }

    return VoidResult<std::string>::ok();
}

Result<size_t, std::string> SyncFence::wait_any(
    const std::vector<SyncFence*>& fences, int timeout_ms)
{
    if (fences.empty()) {
        return Result<size_t, std::string>::error("No fences to wait on");
    }

    // Check for already-signaled
    for (size_t i = 0; i < fences.size(); ++i) {
        if (fences[i]->state() == FenceState::Signaled) {
            return Result<size_t, std::string>::ok(i);
        }
    }

    std::vector<struct pollfd> pfds(fences.size());
    for (size_t i = 0; i < fences.size(); ++i) {
        pfds[i].fd = fences[i]->fd();
        pfds[i].events = POLLIN;
    }

    int rc = ::poll(pfds.data(), pfds.size(), timeout_ms);
    if (rc < 0) {
        return Result<size_t, std::string>::error(
            std::string("poll() failed: ") + ::strerror(errno));
    }
    if (rc == 0) {
        return Result<size_t, std::string>::error("wait_any timed out");
    }

    for (size_t i = 0; i < fences.size(); ++i) {
        if (pfds[i].revents & POLLIN) {
            uint64_t val = 0;
            ::read(pfds[i].fd, &val, sizeof(val));
            return Result<size_t, std::string>::ok(i);
        }
    }

    return Result<size_t, std::string>::error("No fence became ready");
}

// ── Callback ────────────────────────────────────────────────────

void SyncFence::on_signal(std::function<void(const FenceId&)> callback) {
    callback_ = std::move(callback);
}

} // namespace straylight::sync
