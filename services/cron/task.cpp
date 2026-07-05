// services/cron/task.cpp
// Task serialization and schedule parsing.
#include "task.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>

namespace straylight {

// ---------------------------------------------------------------------------
// Schedule parsing
// ---------------------------------------------------------------------------

Result<int64_t, std::string> TaskSerializer::parse_schedule(const std::string& spec) {
    // "every Ns" / "every Nm" / "every Nh" / "every Nd"
    std::regex every_re(R"(every\s+(\d+)\s*(s|m|h|d))", std::regex::icase);
    std::smatch match;
    if (std::regex_search(spec, match, every_re)) {
        int64_t val = std::stoll(match[1].str());
        char unit = static_cast<char>(std::tolower(static_cast<unsigned char>(match[2].str()[0])));
        switch (unit) {
            case 's': return Result<int64_t, std::string>::ok(val);
            case 'm': return Result<int64_t, std::string>::ok(val * 60);
            case 'h': return Result<int64_t, std::string>::ok(val * 3600);
            case 'd': return Result<int64_t, std::string>::ok(val * 86400);
            default: break;
        }
    }

    // "daily HH:MM" — treated as every 24h (interval-based, not clock-aligned for simplicity)
    std::regex daily_re(R"(daily\s+(\d{1,2}):(\d{2}))", std::regex::icase);
    if (std::regex_search(spec, match, daily_re)) {
        return Result<int64_t, std::string>::ok(int64_t(86400));
    }

    // "weekly" — every 7 days
    if (spec.find("weekly") != std::string::npos) {
        return Result<int64_t, std::string>::ok(int64_t(604800));
    }

    // "hourly" shorthand
    if (spec.find("hourly") != std::string::npos) {
        return Result<int64_t, std::string>::ok(int64_t(3600));
    }

    // Try raw seconds
    try {
        int64_t raw = std::stoll(spec);
        if (raw > 0) return Result<int64_t, std::string>::ok(raw);
    } catch (...) {}

    return Result<int64_t, std::string>::error(
        "Cannot parse schedule: '" + spec + "'. "
        "Use: 'every Ns/Nm/Nh/Nd', 'daily HH:MM', 'hourly', 'weekly'");
}

// ---------------------------------------------------------------------------
// Task -> JSON
// ---------------------------------------------------------------------------

std::string TaskSerializer::to_json(const Task& task) {
    nlohmann::json j;
    j["name"] = task.name;
    j["command"] = task.command;
    j["enabled"] = task.enabled;

    nlohmann::json sched;
    sched["spec"] = task.schedule.spec;
    sched["interval_seconds"] = task.schedule.interval_seconds;
    sched["missed_policy"] = task.schedule.missed_policy;
    j["schedule"] = sched;

    nlohmann::json res;
    res["max_cpu_percent"] = task.resources.max_cpu_percent;
    res["min_free_memory_mb"] = task.resources.min_free_memory_mb;
    res["max_concurrent"] = task.resources.max_concurrent;
    j["resources"] = res;

    j["depends_on"] = task.depends_on;
    j["max_retries"] = task.max_retries;
    j["retry_backoff_base_s"] = task.retry_backoff_base_s;
    j["retry_backoff_max_s"] = task.retry_backoff_max_s;
    j["consecutive_failures"] = task.consecutive_failures;
    j["running"] = task.running;
    j["last_run_at"] = task.last_run_at;
    j["last_run_success"] = task.last_run_success;

    return j.dump(2);
}

// ---------------------------------------------------------------------------
// JSON -> Task
// ---------------------------------------------------------------------------

Result<Task, std::string> TaskSerializer::from_json(const std::string& json_str) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(json_str);
    } catch (const nlohmann::json::parse_error& e) {
        return Result<Task, std::string>::error(
            std::string("JSON parse error: ") + e.what());
    }

    Task task;

    if (!j.contains("name") || !j["name"].is_string()) {
        return Result<Task, std::string>::error("Task missing 'name' field");
    }
    task.name = j["name"].get<std::string>();

    if (!j.contains("command") || !j["command"].is_string()) {
        return Result<Task, std::string>::error("Task missing 'command' field");
    }
    task.command = j["command"].get<std::string>();

    task.enabled = j.value("enabled", true);

    if (j.contains("schedule") && j["schedule"].is_object()) {
        const auto& s = j["schedule"];
        task.schedule.spec = s.value("spec", "");
        task.schedule.interval_seconds = s.value("interval_seconds", int64_t(0));
        task.schedule.missed_policy = s.value("missed_policy", "skip");

        // If interval not set but spec is, parse it
        if (task.schedule.interval_seconds <= 0 && !task.schedule.spec.empty()) {
            auto pr = parse_schedule(task.schedule.spec);
            if (pr.has_value()) {
                task.schedule.interval_seconds = pr.value();
            }
        }
    }

    if (j.contains("resources") && j["resources"].is_object()) {
        const auto& r = j["resources"];
        task.resources.max_cpu_percent = r.value("max_cpu_percent", 80.0);
        task.resources.min_free_memory_mb = r.value("min_free_memory_mb", uint64_t(1024));
        task.resources.max_concurrent = r.value("max_concurrent", 1);
    }

    if (j.contains("depends_on") && j["depends_on"].is_array()) {
        for (const auto& dep : j["depends_on"]) {
            if (dep.is_string()) {
                task.depends_on.push_back(dep.get<std::string>());
            }
        }
    }

    task.max_retries = j.value("max_retries", 0);
    task.retry_backoff_base_s = j.value("retry_backoff_base_s", 5.0);
    task.retry_backoff_max_s = j.value("retry_backoff_max_s", 300.0);
    task.consecutive_failures = j.value("consecutive_failures", 0);
    task.running = j.value("running", false);
    task.last_run_at = j.value("last_run_at", "");
    task.last_run_success = j.value("last_run_success", false);

    return Result<Task, std::string>::ok(std::move(task));
}

// ---------------------------------------------------------------------------
// TaskRun -> JSON
// ---------------------------------------------------------------------------

std::string TaskSerializer::run_to_json(const TaskRun& run) {
    nlohmann::json j;
    j["task_name"] = run.task_name;
    j["started_at"] = run.started_at;
    j["finished_at"] = run.finished_at;
    j["exit_code"] = run.exit_code;
    j["stdout"] = run.stdout_capture;
    j["stderr"] = run.stderr_capture;
    j["success"] = run.success;
    j["duration_s"] = run.duration_s;
    return j.dump(2);
}

} // namespace straylight
