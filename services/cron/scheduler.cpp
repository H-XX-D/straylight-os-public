// services/cron/scheduler.cpp
// Smart cron scheduler implementation.
#include "scheduler.h"

#include <straylight/log.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#include <mach/mach.h>
#endif

namespace straylight {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string CronScheduler::now_iso8601() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    gmtime_r(&t, &tm_buf);
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%FT%TZ");
    return oss.str();
}

double CronScheduler::retry_delay(const Task& task) {
    if (task.consecutive_failures <= 0) return 0.0;
    double delay = task.retry_backoff_base_s *
                   std::pow(2.0, static_cast<double>(task.consecutive_failures - 1));
    return std::min(delay, task.retry_backoff_max_s);
}

// ---------------------------------------------------------------------------
// System resources
// ---------------------------------------------------------------------------

SystemResources CronScheduler::get_system_resources() const {
    SystemResources res;

#ifdef __APPLE__
    // CPU load via sysctl
    {
        struct {
            uint32_t count;
        } cpu_info;
        int mib[2] = {CTL_HW, HW_NCPU};
        size_t len = sizeof(cpu_info.count);
        if (sysctl(mib, 2, &cpu_info.count, &len, nullptr, 0) == 0) {
            // Use host_statistics to get CPU usage
            host_cpu_load_info_data_t cpu_load;
            mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
            if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO,
                               reinterpret_cast<host_info_t>(&cpu_load), &count) == KERN_SUCCESS) {
                uint64_t total = 0;
                for (int i = 0; i < CPU_STATE_MAX; ++i) {
                    total += cpu_load.cpu_ticks[i];
                }
                uint64_t idle = cpu_load.cpu_ticks[CPU_STATE_IDLE];
                if (total > 0) {
                    res.cpu_percent = 100.0 * (1.0 - static_cast<double>(idle) / static_cast<double>(total));
                }
            }
        }
    }

    // Free memory via mach
    {
        vm_statistics64_data_t vm_stat;
        mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
        if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                             reinterpret_cast<host_info64_t>(&vm_stat), &count) == KERN_SUCCESS) {
            uint64_t page_size = 0;
            {
                int mib[2] = {CTL_HW, HW_PAGESIZE};
                size_t sz = sizeof(page_size);
                sysctl(mib, 2, &page_size, &sz, nullptr, 0);
            }
            uint64_t free_pages = static_cast<uint64_t>(vm_stat.free_count) +
                                  static_cast<uint64_t>(vm_stat.inactive_count);
            res.free_memory_mb = (free_pages * page_size) / (1024 * 1024);
        }
    }
#else
    // Linux: /proc/stat for CPU, /proc/meminfo for memory
    {
        std::ifstream stat("/proc/stat");
        if (stat.is_open()) {
            std::string line;
            std::getline(stat, line);
            std::istringstream iss(line);
            std::string cpu_label;
            uint64_t user, nice, system, idle, iowait, irq, softirq;
            iss >> cpu_label >> user >> nice >> system >> idle >> iowait >> irq >> softirq;
            uint64_t total = user + nice + system + idle + iowait + irq + softirq;
            if (total > 0) {
                res.cpu_percent = 100.0 * (1.0 - static_cast<double>(idle) / static_cast<double>(total));
            }
        }
    }
    {
        std::ifstream meminfo("/proc/meminfo");
        if (meminfo.is_open()) {
            std::string line;
            while (std::getline(meminfo, line)) {
                if (line.find("MemAvailable:") == 0) {
                    std::istringstream iss(line);
                    std::string label;
                    uint64_t kb;
                    iss >> label >> kb;
                    res.free_memory_mb = kb / 1024;
                    break;
                }
            }
        }
    }
#endif

    return res;
}

bool CronScheduler::resources_available(const Task& task,
                                         const SystemResources& res) const {
    if (res.cpu_percent > task.resources.max_cpu_percent) return false;
    if (res.free_memory_mb < task.resources.min_free_memory_mb) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Dependency checking
// ---------------------------------------------------------------------------

bool CronScheduler::dependencies_met(const Task& task) const {
    for (const auto& dep_name : task.depends_on) {
        auto it = tasks_.find(dep_name);
        if (it == tasks_.end()) return false; // Unknown dependency = not met
        if (!it->second.last_run_success) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Task execution
// ---------------------------------------------------------------------------

TaskRun CronScheduler::execute_task(const Task& task) {
    TaskRun run;
    run.task_name = task.name;
    run.started_at = now_iso8601();

    auto start = std::chrono::steady_clock::now();

    // Create pipes for stdout/stderr capture
    int stdout_pipe[2];
    int stderr_pipe[2];
    if (pipe(stdout_pipe) < 0 || pipe(stderr_pipe) < 0) {
        run.exit_code = -1;
        run.stderr_capture = "pipe() failed";
        run.finished_at = now_iso8601();
        return run;
    }

    pid_t pid = fork();
    if (pid < 0) {
        run.exit_code = -1;
        run.stderr_capture = "fork() failed";
        run.finished_at = now_iso8601();
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        close(stderr_pipe[0]); close(stderr_pipe[1]);
        return run;
    }

    if (pid == 0) {
        // Child process
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stdout_pipe[1]);
        close(stderr_pipe[1]);

        execl("/bin/sh", "sh", "-c", task.command.c_str(), nullptr);
        _exit(127); // exec failed
    }

    // Parent process
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    // Read captured output
    auto read_pipe = [](int fd) -> std::string {
        std::string result;
        char buf[4096];
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            result.append(buf, static_cast<size_t>(n));
        }
        close(fd);
        // Limit capture size to 64KB
        if (result.size() > 65536) {
            result.resize(65536);
            result += "\n... (output truncated at 64KB)";
        }
        return result;
    };

    run.stdout_capture = read_pipe(stdout_pipe[0]);
    run.stderr_capture = read_pipe(stderr_pipe[0]);

    // Wait for child
    int status = 0;
    waitpid(pid, &status, 0);

    auto end = std::chrono::steady_clock::now();
    run.duration_s = std::chrono::duration<double>(end - start).count();
    run.finished_at = now_iso8601();

    if (WIFEXITED(status)) {
        run.exit_code = WEXITSTATUS(status);
        run.success = (run.exit_code == 0);
    } else if (WIFSIGNALED(status)) {
        run.exit_code = -WTERMSIG(status);
        run.success = false;
    } else {
        run.exit_code = -1;
        run.success = false;
    }

    return run;
}

// ---------------------------------------------------------------------------
// History
// ---------------------------------------------------------------------------

void CronScheduler::record_run(const TaskRun& run) {
    auto& hist = history_[run.task_name];
    hist.push_back(run);
    while (hist.size() > kMaxHistoryPerTask) {
        hist.pop_front();
    }
}

std::vector<TaskRun> CronScheduler::get_history(const std::string& name, int limit) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = history_.find(name);
    if (it == history_.end()) return {};

    const auto& hist = it->second;
    int count = std::min(limit, static_cast<int>(hist.size()));
    return std::vector<TaskRun>(hist.end() - count, hist.end());
}

// ---------------------------------------------------------------------------
// Task management
// ---------------------------------------------------------------------------

Result<int, std::string> CronScheduler::load_tasks(const std::string& config_path) {
    std::ifstream in(config_path);
    if (!in) {
        return Result<int, std::string>::error("Cannot open config: " + config_path);
    }

    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(in);
    } catch (const nlohmann::json::parse_error& e) {
        return Result<int, std::string>::error(
            std::string("Config parse error: ") + e.what());
    }

    std::lock_guard<std::mutex> lk(mu_);
    config_path_ = config_path;
    int loaded = 0;

    if (doc.contains("tasks") && doc["tasks"].is_array()) {
        for (const auto& t : doc["tasks"]) {
            auto tr = TaskSerializer::from_json(t.dump());
            if (tr.has_value()) {
                Task task = std::move(tr).value();
                // Set initial next_run to now
                task.schedule.next_run = std::chrono::system_clock::now();
                tasks_[task.name] = std::move(task);
                ++loaded;
            }
        }
    }

    return Result<int, std::string>::ok(loaded);
}

Result<void, std::string> CronScheduler::save_tasks(const std::string& config_path) const {
    std::lock_guard<std::mutex> lk(mu_);

    nlohmann::json doc;
    nlohmann::json tasks_arr = nlohmann::json::array();
    for (const auto& [name, task] : tasks_) {
        auto j = nlohmann::json::parse(TaskSerializer::to_json(task));
        tasks_arr.push_back(std::move(j));
    }
    doc["tasks"] = std::move(tasks_arr);

    std::string path = config_path.empty() ? config_path_ : config_path;
    if (path.empty()) {
        return Result<void, std::string>::error("No config path set");
    }

    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        return Result<void, std::string>::error("Cannot write to " + path);
    }
    out << doc.dump(2) << "\n";
    out.flush();
    if (!out) {
        return Result<void, std::string>::error("Write failed to " + path);
    }

    return Result<void, std::string>::ok();
}

Result<void, std::string> CronScheduler::add_task(Task task) {
    if (task.name.empty()) {
        return Result<void, std::string>::error("Task name cannot be empty");
    }
    if (task.command.empty()) {
        return Result<void, std::string>::error("Task command cannot be empty");
    }

    // Parse schedule if needed
    if (task.schedule.interval_seconds <= 0 && !task.schedule.spec.empty()) {
        auto pr = TaskSerializer::parse_schedule(task.schedule.spec);
        if (!pr.has_value()) {
            return Result<void, std::string>::error(pr.error());
        }
        task.schedule.interval_seconds = pr.value();
    }

    task.schedule.next_run = std::chrono::system_clock::now() +
        std::chrono::seconds(task.schedule.interval_seconds);

    std::lock_guard<std::mutex> lk(mu_);
    if (tasks_.count(task.name)) {
        return Result<void, std::string>::error("Task already exists: " + task.name);
    }
    tasks_[task.name] = std::move(task);

    return Result<void, std::string>::ok();
}

Result<void, std::string> CronScheduler::remove_task(const std::string& name) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = tasks_.find(name);
    if (it == tasks_.end()) {
        return Result<void, std::string>::error("Task not found: " + name);
    }
    if (it->second.running) {
        return Result<void, std::string>::error("Task is currently running: " + name);
    }
    tasks_.erase(it);
    return Result<void, std::string>::ok();
}

Result<void, std::string> CronScheduler::set_enabled(const std::string& name, bool enabled) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = tasks_.find(name);
    if (it == tasks_.end()) {
        return Result<void, std::string>::error("Task not found: " + name);
    }
    it->second.enabled = enabled;
    return Result<void, std::string>::ok();
}

std::vector<Task> CronScheduler::list_tasks() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<Task> result;
    result.reserve(tasks_.size());
    for (const auto& [_, task] : tasks_) {
        result.push_back(task);
    }
    return result;
}

Result<Task, std::string> CronScheduler::get_task(const std::string& name) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = tasks_.find(name);
    if (it == tasks_.end()) {
        return Result<Task, std::string>::error("Task not found: " + name);
    }
    return Result<Task, std::string>::ok(it->second);
}

// ---------------------------------------------------------------------------
// Run immediately
// ---------------------------------------------------------------------------

Result<TaskRun, std::string> CronScheduler::run_now(const std::string& name) {
    Task task;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = tasks_.find(name);
        if (it == tasks_.end()) {
            return Result<TaskRun, std::string>::error("Task not found: " + name);
        }
        if (it->second.running) {
            return Result<TaskRun, std::string>::error("Task already running: " + name);
        }
        it->second.running = true;
        task = it->second;
    }

    TaskRun run = execute_task(task);

    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = tasks_.find(name);
        if (it != tasks_.end()) {
            it->second.running = false;
            it->second.last_run_at = run.finished_at;
            it->second.last_run_success = run.success;
            if (run.success) {
                it->second.consecutive_failures = 0;
            } else {
                it->second.consecutive_failures++;
            }
            // Reschedule
            it->second.schedule.next_run = std::chrono::system_clock::now() +
                std::chrono::seconds(it->second.schedule.interval_seconds);
        }
        record_run(run);
    }

    return Result<TaskRun, std::string>::ok(std::move(run));
}

// ---------------------------------------------------------------------------
// Scheduler tick
// ---------------------------------------------------------------------------

Result<int, std::string> CronScheduler::tick() {
    auto now = std::chrono::system_clock::now();
    auto sys_res = get_system_resources();

    std::vector<std::string> due_tasks;

    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& [name, task] : tasks_) {
            if (!task.enabled) continue;
            if (task.running) continue;
            if (task.schedule.interval_seconds <= 0) continue;
            if (now < task.schedule.next_run) continue;

            // Check retry backoff
            if (task.consecutive_failures > 0 && task.max_retries > 0) {
                if (task.consecutive_failures > task.max_retries) continue;
            }

            due_tasks.push_back(name);
        }
    }

    int executed = 0;
    for (const auto& name : due_tasks) {
        Task task;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = tasks_.find(name);
            if (it == tasks_.end()) continue;

            // Re-check conditions under lock
            if (it->second.running) continue;

            // Check dependencies
            if (!dependencies_met(it->second)) {
                SL_DEBUG("cron: skipping '{}' — dependencies not met", name);
                continue;
            }

            // Check resources
            if (!resources_available(it->second, sys_res)) {
                SL_DEBUG("cron: skipping '{}' — insufficient resources (CPU={:.0f}%, "
                        "FreeMem={}MB)", name, sys_res.cpu_percent, sys_res.free_memory_mb);
                continue;
            }

            it->second.running = true;
            task = it->second;
        }

        SL_INFO("cron: executing task '{}'", name);
        TaskRun run = execute_task(task);

        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = tasks_.find(name);
            if (it != tasks_.end()) {
                it->second.running = false;
                it->second.last_run_at = run.finished_at;
                it->second.last_run_success = run.success;

                if (run.success) {
                    it->second.consecutive_failures = 0;
                    SL_INFO("cron: task '{}' succeeded ({}s)", name, run.duration_s);
                } else {
                    it->second.consecutive_failures++;
                    SL_WARN("cron: task '{}' failed (exit={}, failures={})",
                            name, run.exit_code, it->second.consecutive_failures);
                }

                // Reschedule
                auto interval = std::chrono::seconds(it->second.schedule.interval_seconds);
                if (!run.success && it->second.consecutive_failures <= it->second.max_retries) {
                    // Retry with backoff
                    auto delay_s = static_cast<int64_t>(retry_delay(it->second));
                    it->second.schedule.next_run = std::chrono::system_clock::now() +
                        std::chrono::seconds(delay_s);
                } else {
                    it->second.schedule.next_run = std::chrono::system_clock::now() + interval;
                }
            }
            record_run(run);
        }
        ++executed;
    }

    return Result<int, std::string>::ok(executed);
}

// ---------------------------------------------------------------------------
// Catch-up missed
// ---------------------------------------------------------------------------

int CronScheduler::catch_up_missed() {
    auto now = std::chrono::system_clock::now();
    int caught_up = 0;

    std::vector<std::string> missed_tasks;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& [name, task] : tasks_) {
            if (!task.enabled) continue;
            if (task.running) continue;
            if (task.schedule.missed_policy != "run") continue;

            // If next_run is more than one interval in the past, it's missed
            auto one_interval = std::chrono::seconds(task.schedule.interval_seconds);
            if (now > task.schedule.next_run + one_interval) {
                missed_tasks.push_back(name);
            }
        }
    }

    for (const auto& name : missed_tasks) {
        SL_INFO("cron: catching up missed task '{}'", name);
        auto r = run_now(name);
        if (r.has_value()) ++caught_up;
    }

    return caught_up;
}

} // namespace straylight
