// lib/common/include/straylight/daemon.h
#pragma once

#include <straylight/result.h>
#include <straylight/error.h>
#include <straylight/config.h>
#include <straylight/log.h>
#include <straylight/export.h>
#include <atomic>
#include <csignal>
#include <unistd.h>

namespace straylight {

/// Base class for long-running daemon processes.
/// Subclasses implement init(), tick(), and shutdown().
class STRAYLIGHT_EXPORT DaemonBase {
public:
    virtual ~DaemonBase() = default;

    /// Called once after signal handlers are installed.
    virtual Result<void, SLError> init(const Config& cfg) = 0;

    /// Called in a loop until shutdown requested.
    virtual Result<void, SLError> tick() = 0;

    /// Called on SIGTERM/SIGINT before process exit.
    virtual void shutdown() = 0;

    /// Blocks until shutdown signal; calls tick() in loop.
    int run(const Config& cfg);

protected:
    bool shutdown_requested() const { return g_shutdown_.load(); }

private:
    static std::atomic<bool> g_shutdown_;
};

} // namespace straylight
