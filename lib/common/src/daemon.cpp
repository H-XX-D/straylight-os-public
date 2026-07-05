// lib/common/src/daemon.cpp
#include <straylight/daemon.h>

namespace straylight {

std::atomic<bool> DaemonBase::g_shutdown_{false};

int DaemonBase::run(const Config& cfg) {
    // Reset shutdown flag — allows reuse in tests or monolith mode.
    // Only one DaemonBase may call run() per process (signal handler is global).
    g_shutdown_.store(false);

    struct sigaction sa{};
    sa.sa_handler = [](int) { g_shutdown_.store(true); };
    sa.sa_flags = SA_RESTART;  // Don't interrupt sleep_for/read/write with EINTR
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT,  &sa, nullptr);

    auto r = init(cfg);
    if (!r.has_value()) {
        SL_ERROR("daemon init failed: {}", r.error().message());
        return 1;
    }
    SL_INFO("daemon started (pid={})", getpid());

    while (!g_shutdown_.load()) {
        auto tr = tick();
        if (!tr.has_value()) {
            SL_ERROR("tick error: {}", tr.error().message());
            break;
        }
    }
    shutdown();
    SL_INFO("daemon stopped");
    return 0;
}

} // namespace straylight
