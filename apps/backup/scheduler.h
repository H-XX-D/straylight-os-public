// apps/backup/scheduler.h
// Cron-like backup scheduler with a background timer thread
#pragma once

#include "engine.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace straylight::backup {

/// Schedule interval type.
enum class Interval { Hourly, Daily, Weekly, Custom };

/// One schedule entry — binds an interval to a named BackupProfile.
struct Schedule {
    std::string profile_name;
    Interval interval        = Interval::Daily;
    std::chrono::seconds custom_interval{0};
    int hour    = 2;  // for Daily/Weekly: preferred hour of day (0-23)
    int weekday = 0;  // for Weekly: 0=Sunday … 6=Saturday
    bool enabled = true;
    std::chrono::system_clock::time_point last_run{};
};

/// Manages a collection of schedules and runs a background thread that
/// checks every 60 seconds whether any schedule is due.
class Scheduler {
public:
    ~Scheduler() { stop(); }

    /// Load schedules from `~/.config/straylight/backup-schedules.json`.
    Result<void, SLError> load();

    /// Persist schedules to disk.
    Result<void, SLError> save() const;

    void add(Schedule s);
    void remove(const std::string& profile_name);

    /// Start the background ticker thread.
    void start(Engine& engine);

    /// Stop the background thread (blocks until it exits).
    void stop();

    [[nodiscard]] const std::vector<Schedule>& schedules() const { return scheds_; }

    /// Return the names of profiles whose scheduled run has been missed since
    /// the last time they were run.
    [[nodiscard]] std::vector<std::string> overdue() const;

private:
    std::vector<Schedule>  scheds_;
    std::atomic<bool>      running_{false};
    std::thread            thread_;
    mutable std::mutex     mtx_;

    /// Return true if schedule `s` is due to run right now.
    [[nodiscard]] bool is_due(const Schedule& s) const;

    static fs::path schedules_path();
};

} // namespace straylight::backup
