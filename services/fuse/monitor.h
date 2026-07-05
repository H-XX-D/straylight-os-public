/**
 * StrayLight Fuse — Process Health Monitor
 *
 * Monitors fused processes for crashes and unexpected exits.
 * Automatically cleans up shared regions and sessions on process death.
 */
#pragma once

#include "fusion_engine.h"
#include "straylight/result.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_set>
#include <vector>

namespace straylight::fuse {

/** Event types emitted by the monitor. */
enum class MonitorEvent {
    ProcessDied,
    SessionCleaned,
    RegionOrphaned
};

struct MonitorAlert {
    MonitorEvent event;
    std::string  session_id;
    pid_t        pid{0};
    std::string  message;
    std::chrono::steady_clock::time_point timestamp;
};

class FuseMonitor {
public:
    using AlertCallback = std::function<void(const MonitorAlert&)>;

    explicit FuseMonitor(FusionEngine& engine)
        : engine_(engine) {}

    /** Register a callback for monitor alerts. */
    void on_alert(AlertCallback cb) {
        std::lock_guard<std::mutex> lock(mu_);
        callbacks_.push_back(std::move(cb));
    }

    /**
     * Run one monitoring cycle. Call this from the daemon's tick().
     * Checks all fused processes and reaps dead sessions.
     */
    void check() {
        auto sessions = engine_.list_sessions();
        std::unordered_set<pid_t> dead_pids;

        for (const auto& session : sessions) {
            bool p1_alive = process_alive(session.pid1);
            bool p2_alive = process_alive(session.pid2);

            if (!p1_alive || !p2_alive) {
                // At least one process died
                if (!p1_alive) {
                    if (dead_pids.insert(session.pid1).second) {
                        emit_alert({
                            MonitorEvent::ProcessDied,
                            session.session_id,
                            session.pid1,
                            "Process " + std::to_string(session.pid1) +
                                " died while fused",
                            std::chrono::steady_clock::now()
                        });
                    }
                }
                if (!p2_alive) {
                    if (dead_pids.insert(session.pid2).second) {
                        emit_alert({
                            MonitorEvent::ProcessDied,
                            session.session_id,
                            session.pid2,
                            "Process " + std::to_string(session.pid2) +
                                " died while fused",
                            std::chrono::steady_clock::now()
                        });
                    }
                }
            }
        }

        // Let the engine clean up dead sessions
        size_t before = engine_.list_sessions().size();
        engine_.reap_dead_sessions();
        size_t after = engine_.list_sessions().size();

        if (after < before) {
            size_t cleaned = before - after;
            emit_alert({
                MonitorEvent::SessionCleaned,
                "",
                0,
                "Cleaned " + std::to_string(cleaned) +
                    " dead fusion session(s)",
                std::chrono::steady_clock::now()
            });
        }

        ++check_count_;
    }

    /** Number of check cycles completed. */
    uint64_t check_count() const { return check_count_.load(); }

    /** Get recent alerts (last N). */
    std::vector<MonitorAlert> recent_alerts(size_t max_count = 50) const {
        std::lock_guard<std::mutex> lock(mu_);
        size_t start = 0;
        if (alerts_.size() > max_count)
            start = alerts_.size() - max_count;
        return std::vector<MonitorAlert>(
            alerts_.begin() + static_cast<ptrdiff_t>(start),
            alerts_.end());
    }

private:
    FusionEngine& engine_;
    mutable std::mutex mu_;
    std::vector<AlertCallback> callbacks_;
    std::vector<MonitorAlert> alerts_;
    std::atomic<uint64_t> check_count_{0};

    bool process_alive(pid_t pid) const {
        return kill(pid, 0) == 0;
    }

    void emit_alert(MonitorAlert alert) {
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& cb : callbacks_) {
            cb(alert);
        }
        alerts_.push_back(std::move(alert));

        // Cap alert history at 1000
        if (alerts_.size() > 1000) {
            alerts_.erase(alerts_.begin(),
                          alerts_.begin() + static_cast<ptrdiff_t>(
                              alerts_.size() - 500));
        }
    }
};

} // namespace straylight::fuse
