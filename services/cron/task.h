// services/cron/task.h
// Task definition and execution history for StrayLight Cron.
#pragma once

#include <straylight/result.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace straylight {

/// Execution result for a task run.
struct TaskRun {
    std::string task_name;
    std::string started_at;   // ISO 8601
    std::string finished_at;  // ISO 8601
    int exit_code = -1;
    std::string stdout_capture;
    std::string stderr_capture;
    bool success = false;
    double duration_s = 0.0;
};

/// Resource constraints for task execution.
struct TaskResources {
    double max_cpu_percent = 80.0;     // Don't start if CPU > this
    uint64_t min_free_memory_mb = 1024; // Don't start if free RAM < this
    int max_concurrent = 1;             // Max concurrent instances of this task
};

/// Schedule specification.
struct TaskSchedule {
    /// Human-readable schedule string: "every 1h", "every 30m", "daily 03:00"
    std::string spec;

    /// Parsed interval in seconds (0 = one-shot or cron-style).
    int64_t interval_seconds = 0;

    /// Next scheduled run time.
    std::chrono::system_clock::time_point next_run;

    /// Catch-up policy: "skip" (skip missed runs) or "run" (run immediately).
    std::string missed_policy = "skip";
};

/// A scheduled task definition.
struct Task {
    std::string name;
    std::string command;
    TaskSchedule schedule;
    TaskResources resources;
    std::vector<std::string> depends_on;  // Names of tasks that must succeed first
    bool enabled = true;

    /// Retry configuration.
    int max_retries = 0;
    double retry_backoff_base_s = 5.0;
    double retry_backoff_max_s = 300.0;

    /// Current state.
    int consecutive_failures = 0;
    bool running = false;
    std::string last_run_at;
    bool last_run_success = false;
};

/// Task serialization to/from JSON.
class TaskSerializer {
public:
    /// Serialize a task to JSON string.
    static std::string to_json(const Task& task);

    /// Deserialize a task from JSON string.
    static Result<Task, std::string> from_json(const std::string& json_str);

    /// Serialize a TaskRun to JSON string.
    static std::string run_to_json(const TaskRun& run);

    /// Parse a schedule spec into interval seconds.
    static Result<int64_t, std::string> parse_schedule(const std::string& spec);
};

} // namespace straylight
