/**
 * StrayLight Sync Kernel — TimelineSemaphore implementation
 *
 * Uses eventfd + condition_variable for efficient waiting.
 * The eventfd enables cross-process notification (via fd passing),
 * while the condition_variable handles in-process multi-waiter wake-up.
 */
#include "straylight/sync/timeline.h"

#include <sys/eventfd.h>
#include <poll.h>
#include <unistd.h>
#include <errno.h>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace straylight::sync {

// Internal condition variable for in-process waiters
struct TimelineInternal {
    std::mutex cv_mutex;
    std::condition_variable cv;
};

static thread_local TimelineInternal* tl_internal = nullptr;

// ── Construction / Destruction ──────────────────────────────────

TimelineSemaphore::TimelineSemaphore() = default;

TimelineSemaphore::~TimelineSemaphore() {
    if (event_fd_ >= 0) {
        ::close(event_fd_);
    }
}

TimelineSemaphore::TimelineSemaphore(TimelineSemaphore&& o) noexcept
    : event_fd_(o.event_fd_)
    , name_(std::move(o.name_))
    , value_(o.value_.load())
    , max_value_(o.max_value_)
{
    std::lock_guard<std::mutex> lock(o.waiters_mutex_);
    waiters_ = std::move(o.waiters_);
    o.event_fd_ = -1;
}

TimelineSemaphore& TimelineSemaphore::operator=(TimelineSemaphore&& o) noexcept {
    if (this != &o) {
        if (event_fd_ >= 0) ::close(event_fd_);
        event_fd_ = o.event_fd_;
        name_ = std::move(o.name_);
        value_.store(o.value_.load());
        max_value_ = o.max_value_;
        std::lock_guard<std::mutex> lock(o.waiters_mutex_);
        waiters_ = std::move(o.waiters_);
        o.event_fd_ = -1;
    }
    return *this;
}

// ── Factory ─────────────────────────────────────────────────────

Result<TimelineSemaphore, std::string> TimelineSemaphore::create(
    const TimelineCreateInfo& info)
{
    int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd < 0) {
        return Result<TimelineSemaphore, std::string>::error(
            std::string("eventfd() failed: ") + ::strerror(errno));
    }

    TimelineSemaphore tl;
    tl.event_fd_ = fd;
    tl.name_ = info.name.empty() ? "timeline" : info.name;
    tl.value_.store(info.initial_value);
    tl.max_value_ = info.max_value;

    return Result<TimelineSemaphore, std::string>::ok(std::move(tl));
}

Result<TimelineSemaphore, std::string> TimelineSemaphore::import_fd(
    int fd, const std::string& name)
{
    if (fd < 0) {
        return Result<TimelineSemaphore, std::string>::error("Invalid fd");
    }

    TimelineSemaphore tl;
    tl.event_fd_ = fd;
    tl.name_ = name;

    return Result<TimelineSemaphore, std::string>::ok(std::move(tl));
}

// ── Signal ──────────────────────────────────────────────────────

VoidResult<std::string> TimelineSemaphore::signal(uint64_t new_value) {
    uint64_t current = value_.load(std::memory_order_acquire);

    if (new_value <= current) {
        return VoidResult<std::string>::error(
            "Timeline value must advance: current=" + std::to_string(current)
            + " requested=" + std::to_string(new_value));
    }

    if (new_value > max_value_) {
        return VoidResult<std::string>::error(
            "Timeline value " + std::to_string(new_value)
            + " exceeds max " + std::to_string(max_value_));
    }

    value_.store(new_value, std::memory_order_release);

    // Update stats
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        signal_count_++;
    }

    // Notify eventfd for cross-process waiters
    uint64_t val = 1;
    ::write(event_fd_, &val, sizeof(val));

    // Fire registered callbacks
    {
        std::lock_guard<std::mutex> lock(waiters_mutex_);
        auto it = waiters_.begin();
        while (it != waiters_.end()) {
            if (new_value >= it->target) {
                if (it->callback) {
                    it->callback(new_value);
                }
                it = waiters_.erase(it);
            } else {
                ++it;
            }
        }
    }

    return VoidResult<std::string>::ok();
}

Result<uint64_t, std::string> TimelineSemaphore::increment() {
    uint64_t new_val = value_.load(std::memory_order_acquire) + 1;
    auto res = signal(new_val);
    if (!res.has_value()) {
        return Result<uint64_t, std::string>::error(res.error());
    }
    return Result<uint64_t, std::string>::ok(new_val);
}

// ── Wait ────────────────────────────────────────────────────────

VoidResult<std::string> TimelineSemaphore::wait(uint64_t target, int timeout_ms) {
    // Fast path
    if (value_.load(std::memory_order_acquire) >= target) {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        wait_count_++;
        return VoidResult<std::string>::ok();
    }

    auto start = std::chrono::steady_clock::now();
    auto deadline = start + std::chrono::milliseconds(
        timeout_ms < 0 ? INT32_MAX : timeout_ms);

    // Poll the eventfd, re-checking the value after each wake-up
    while (true) {
        if (value_.load(std::memory_order_acquire) >= target) {
            auto elapsed = std::chrono::duration<double, std::micro>(
                std::chrono::steady_clock::now() - start).count();
            std::lock_guard<std::mutex> lock(stats_mutex_);
            wait_count_++;
            total_wait_us_ += elapsed;
            if (elapsed > max_wait_us_) max_wait_us_ = elapsed;
            return VoidResult<std::string>::ok();
        }

        int time_left = -1;
        if (timeout_ms >= 0) {
            auto now = std::chrono::steady_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - now).count();
            if (ms <= 0) {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                timeout_count_++;
                return VoidResult<std::string>::error(
                    "Timeline wait timed out (target=" + std::to_string(target)
                    + " current=" + std::to_string(value_.load()) + ")");
            }
            time_left = static_cast<int>(ms);
        }

        struct pollfd pfd{};
        pfd.fd = event_fd_;
        pfd.events = POLLIN;

        int rc = ::poll(&pfd, 1, std::min(time_left, 100)); // 100ms max poll
        if (rc > 0) {
            // Drain eventfd
            uint64_t val = 0;
            ::read(event_fd_, &val, sizeof(val));
        }
    }
}

// ── Callbacks ───────────────────────────────────────────────────

void TimelineSemaphore::on_reach(uint64_t target,
                                   std::function<void(uint64_t)> callback)
{
    // If already reached, fire immediately
    if (value_.load(std::memory_order_acquire) >= target) {
        callback(value_.load());
        return;
    }

    std::lock_guard<std::mutex> lock(waiters_mutex_);
    waiters_.push_back({target, std::move(callback)});
}

// ── Stats ───────────────────────────────────────────────────────

TimelineStats TimelineSemaphore::stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    TimelineStats s{};
    s.current_value = value_.load();
    s.signal_count = signal_count_;
    s.wait_count = wait_count_;
    s.timeout_count = timeout_count_;
    s.avg_wait_us = wait_count_ > 0 ? total_wait_us_ / wait_count_ : 0.0;
    s.max_wait_us = max_wait_us_;
    return s;
}

} // namespace straylight::sync
