// bin/agent/event_loop.h
#pragma once

#include <straylight/result.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace straylight::agent {

/// Epoll-based event loop for monitoring file descriptors.
/// Callbacks are dispatched outside the lock (copy-under-lock pattern).
class EventLoop {
public:
    EventLoop();
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    /// Register a file descriptor with the given epoll event mask and callback.
    Result<void, std::string> add_fd(int fd, uint32_t events,
                                     std::function<void(uint32_t)> cb);

    /// Unregister a file descriptor.
    Result<void, std::string> remove_fd(int fd);

    /// Wait for events and dispatch callbacks. Returns after timeout_ms or
    /// when events fire. Pass -1 to block indefinitely.
    Result<void, std::string> run_once(int timeout_ms = 100);

    /// Signal the event loop to stop (makes run_once return immediately
    /// on next iteration if used in a loop).
    void stop();

    /// Whether the loop is still running.
    bool running() const { return running_.load(); }

private:
    int epoll_fd_ = -1;
    std::atomic<bool> running_{true};
    std::unordered_map<int, std::function<void(uint32_t)>> handlers_;
    std::mutex mutex_;
};

} // namespace straylight::agent
