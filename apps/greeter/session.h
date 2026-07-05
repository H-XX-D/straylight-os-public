// apps/greeter/session.h
// Wayland ext-session-lock-v1 client
#pragma once

#include <straylight/result.h>
#include <straylight/error.h>

#include <memory>

struct wl_display;
struct wl_surface;

namespace straylight::greeter {

/// Manages the ext-session-lock-v1 protocol for screen locking.
/// Acquires a session lock, creates lock surfaces per output, and
/// releases the lock on successful authentication.
class SessionLock {
public:
    /// Acquire the session lock on the given display.
    /// Creates one lock surface per connected output.
    static Result<SessionLock, SLError> acquire(wl_display* display);

    /// Unlock the session and destroy all lock surfaces.
    void unlock_and_destroy();

    /// Get the lock surface for EGL rendering (first output).
    [[nodiscard]] wl_surface* surface() const;

    /// Get the configured surface dimensions.
    [[nodiscard]] int width() const;
    [[nodiscard]] int height() const;

    /// Returns true if the lock is currently held.
    [[nodiscard]] bool is_locked() const;

    ~SessionLock();
    SessionLock(SessionLock&& other) noexcept;
    SessionLock& operator=(SessionLock&& other) noexcept;

    SessionLock(const SessionLock&) = delete;
    SessionLock& operator=(const SessionLock&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    explicit SessionLock(std::unique_ptr<Impl> impl);
};

} // namespace straylight::greeter
