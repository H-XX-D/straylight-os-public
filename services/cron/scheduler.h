// services/cron/scheduler.h
// Smart task scheduler with dependency awareness and resource constraints.
#pragma once

#include <straylight/result.h>
#include "task.h"

#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace straylight {

/// System resource snapshot.
struct SystemResources {
    double cpu_percent = 0.0;
    uint64_t free_memory_mb = 0;
};

/// Smart cron scheduler — manages task lifecycle, dependencies, and execution.
class CronScheduler {
public:
    /// Maximum history entries per task.
    static constexpr size_t kMaxHistoryPerTask = 100;

    /// Load tasks from a JSON config file.
    Result<int, std::string> load_tasks(const std::string& config_path);

    /// Save tasks to the JSON config file.
    Result<void, std::string> save_tasks(const std::string& config_path) const;

    /// Add a new task.
    Result<void, std::string> add_task(Task task);

    /// Remove a task by name.
    Result<void, std::string> remove_task(const std::string& name);

    /// Enable or disable a task.
    Result<void, std::string> set_enabled(const std::string& name, bool enabled);

    /// Get all tasks.
    std::vector<Task> list_tasks() const;

    /// Get a task by name.
    Result<Task, std::string> get_task(const std::string& name) const;

    /// Get execution history for a task.
    std::vector<TaskRun> get_history(const std::string& name,
                                     int limit = 20) const;

    /// Run a task immediately by name, ignoring schedule.
    Result<TaskRun, std::string> run_now(const std::string& name);

    /// Main scheduler tick — check all tasks, execute those that are due.
    /// Returns the number of tasks executed.
    Result<int, std::string> tick();

    /// Check and run missed tasks according to catch-up policy.
    int catch_up_missed();

private:
    /// Execute a task and capture output.
    TaskRun execute_task(const Task& task);

    /// Check if a task's dependencies are satisfied.
    bool dependencies_met(const Task& task) const;

    /// Get current system resource snapshot.
    SystemResources get_system_resources() const;

    /// Check if resources are sufficient for a task.
    bool resources_available(const Task& task,
                             const SystemResources& res) const;

    /// Calculate retry delay with exponential backoff.
    static double retry_delay(const Task& task);

    /// Get current ISO 8601 timestamp.
    static std::string now_iso8601();

    /// Record a task execution in history.
    void record_run(const TaskRun& run);

    mutable std::mutex mu_;
    std::map<std::string, Task> tasks_;
    std::map<std::string, std::deque<TaskRun>> history_;
    std::string config_path_;
};

} // namespace straylight
