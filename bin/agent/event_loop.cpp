// bin/agent/event_loop.cpp
#include "event_loop.h"

#include <sys/epoll.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <vector>

namespace straylight::agent {

EventLoop::EventLoop() {
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    // If epoll_create1 fails (e.g. not on Linux), epoll_fd_ stays -1.
    // Callers on non-Linux can still construct; run_once will return an error.
}

EventLoop::~EventLoop() {
    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
        epoll_fd_ = -1;
    }
}

Result<void, std::string> EventLoop::add_fd(int fd, uint32_t events,
                                             std::function<void(uint32_t)> cb) {
    if (epoll_fd_ < 0) {
        return Result<void, std::string>::error("epoll not initialized");
    }

    struct epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;

    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        return Result<void, std::string>::error(
            std::string("epoll_ctl ADD failed: ") + std::strerror(errno));
    }

    std::lock_guard lock(mutex_);
    handlers_[fd] = std::move(cb);
    return Result<void, std::string>::ok();
}

Result<void, std::string> EventLoop::remove_fd(int fd) {
    if (epoll_fd_ < 0) {
        return Result<void, std::string>::error("epoll not initialized");
    }

    if (epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) < 0) {
        return Result<void, std::string>::error(
            std::string("epoll_ctl DEL failed: ") + std::strerror(errno));
    }

    std::lock_guard lock(mutex_);
    handlers_.erase(fd);
    return Result<void, std::string>::ok();
}

Result<void, std::string> EventLoop::run_once(int timeout_ms) {
    if (epoll_fd_ < 0) {
        return Result<void, std::string>::error("epoll not initialized");
    }
    if (!running_.load()) {
        return Result<void, std::string>::ok();
    }

    static constexpr int kMaxEvents = 64;
    std::vector<struct epoll_event> events(kMaxEvents);

    int n = epoll_wait(epoll_fd_, events.data(), kMaxEvents, timeout_ms);
    if (n < 0) {
        if (errno == EINTR) {
            // Interrupted by signal — not an error.
            return Result<void, std::string>::ok();
        }
        return Result<void, std::string>::error(
            std::string("epoll_wait failed: ") + std::strerror(errno));
    }

    // Copy-under-lock: snapshot the handlers we need, then invoke outside the lock.
    std::vector<std::pair<uint32_t, std::function<void(uint32_t)>>> dispatches;
    dispatches.reserve(static_cast<size_t>(n));

    {
        std::lock_guard lock(mutex_);
        for (int i = 0; i < n; ++i) {
            int fd = events[static_cast<size_t>(i)].data.fd;
            uint32_t ev = events[static_cast<size_t>(i)].events;
            auto it = handlers_.find(fd);
            if (it != handlers_.end()) {
                dispatches.emplace_back(ev, it->second);
            }
        }
    }

    for (auto& [ev, handler] : dispatches) {
        handler(ev);
    }

    return Result<void, std::string>::ok();
}

void EventLoop::stop() {
    running_.store(false);
}

} // namespace straylight::agent
