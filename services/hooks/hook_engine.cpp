// services/hooks/hook_engine.cpp
#include "hook_engine.h"

#include <straylight/log.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

namespace straylight {

namespace fs = std::filesystem;

namespace {

std::string timestamp_now() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

} // namespace

std::string HookEngine::event_name(SystemEvent event) {
    switch (event) {
        case SystemEvent::Boot:          return "boot";
        case SystemEvent::Shutdown:      return "shutdown";
        case SystemEvent::Suspend:       return "suspend";
        case SystemEvent::Resume:        return "resume";
        case SystemEvent::NetworkUp:     return "network-up";
        case SystemEvent::NetworkDown:   return "network-down";
        case SystemEvent::UsbAttach:     return "usb-attach";
        case SystemEvent::UsbDetach:     return "usb-detach";
        case SystemEvent::LidOpen:       return "lid-open";
        case SystemEvent::LidClose:      return "lid-close";
        case SystemEvent::BatteryLow:    return "battery-low";
        case SystemEvent::PowerAC:       return "power-ac";
        case SystemEvent::PowerBattery:  return "power-battery";
    }
    return "unknown";
}

Result<SystemEvent, std::string> HookEngine::parse_event(const std::string& name) {
    static const std::map<std::string, SystemEvent> events = {
        {"boot",           SystemEvent::Boot},
        {"shutdown",       SystemEvent::Shutdown},
        {"suspend",        SystemEvent::Suspend},
        {"resume",         SystemEvent::Resume},
        {"network-up",     SystemEvent::NetworkUp},
        {"network-down",   SystemEvent::NetworkDown},
        {"usb-attach",     SystemEvent::UsbAttach},
        {"usb-detach",     SystemEvent::UsbDetach},
        {"lid-open",       SystemEvent::LidOpen},
        {"lid-close",      SystemEvent::LidClose},
        {"battery-low",    SystemEvent::BatteryLow},
        {"power-ac",       SystemEvent::PowerAC},
        {"power-battery",  SystemEvent::PowerBattery},
    };

    auto it = events.find(name);
    if (it == events.end()) {
        return Result<SystemEvent, std::string>::error("Unknown event: " + name);
    }
    return Result<SystemEvent, std::string>::ok(it->second);
}

std::string HookEngine::generate_id() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(10000, 99999);
    return "hook-" + std::to_string(dist(gen));
}

Result<void, SLError> HookEngine::load_hooks(const fs::path& hooks_dir) {
    hooks_dir_ = hooks_dir;
    if (!fs::exists(hooks_dir)) {
        std::error_code ec;
        fs::create_directories(hooks_dir, ec);
        return Result<void, SLError>::ok();
    }

    for (const auto& entry : fs::directory_iterator(hooks_dir)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        if (ext != ".json") continue;

        std::ifstream ifs(entry.path());
        if (!ifs) continue;

        try {
            nlohmann::json j;
            ifs >> j;

            Hook hook;
            hook.id = j.value("id", entry.path().stem().string());
            hook.script_path = j.value("script", "");
            hook.timeout_seconds = j.value("timeout_seconds", 30);
            hook.enabled = j.value("enabled", true);
            hook.priority = j.value("priority", 50);

            std::string event_str = j.value("event", "");
            auto ev = parse_event(event_str);
            if (!ev.has_value()) {
                SL_WARN("hooks: unknown event '{}' in {}", event_str, entry.path().string());
                continue;
            }
            hook.event = ev.value();

            hooks_.push_back(std::move(hook));
        } catch (const std::exception& e) {
            SL_WARN("hooks: failed to parse {}: {}", entry.path().string(), e.what());
        }
    }

    // Sort by priority
    std::sort(hooks_.begin(), hooks_.end(),
              [](const Hook& a, const Hook& b) { return a.priority < b.priority; });

    SL_INFO("hooks: loaded {} hooks from {}", hooks_.size(), hooks_dir.string());
    return Result<void, SLError>::ok();
}

Result<void, SLError> HookEngine::add_hook(const Hook& hook) {
    std::lock_guard lock(mutex_);

    // Check for duplicate ID
    for (const auto& h : hooks_) {
        if (h.id == hook.id) {
            return Result<void, SLError>::error(
                SLError{SLErrorCode::AlreadyExists, "Hook already exists: " + hook.id});
        }
    }

    hooks_.push_back(hook);
    std::sort(hooks_.begin(), hooks_.end(),
              [](const Hook& a, const Hook& b) { return a.priority < b.priority; });

    // Save to hooks directory
    if (!hooks_dir_.empty()) {
        nlohmann::json j;
        j["id"] = hook.id;
        j["event"] = event_name(hook.event);
        j["script"] = hook.script_path;
        j["timeout_seconds"] = hook.timeout_seconds;
        j["enabled"] = hook.enabled;
        j["priority"] = hook.priority;

        fs::path path = hooks_dir_ / (hook.id + ".json");
        std::ofstream ofs(path);
        if (ofs) ofs << j.dump(2) << "\n";
    }

    return Result<void, SLError>::ok();
}

Result<void, SLError> HookEngine::remove_hook(const std::string& id) {
    std::lock_guard lock(mutex_);

    auto it = std::find_if(hooks_.begin(), hooks_.end(),
                            [&](const Hook& h) { return h.id == id; });
    if (it == hooks_.end()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::NotFound, "Hook not found: " + id});
    }

    hooks_.erase(it);

    // Remove config file
    if (!hooks_dir_.empty()) {
        fs::path path = hooks_dir_ / (id + ".json");
        std::error_code ec;
        fs::remove(path, ec);
    }

    return Result<void, SLError>::ok();
}

std::vector<Hook> HookEngine::list_hooks(SystemEvent event) const {
    std::lock_guard lock(mutex_);
    if (static_cast<int>(event) == -1) {
        return hooks_;
    }

    std::vector<Hook> result;
    for (const auto& h : hooks_) {
        if (h.event == event) result.push_back(h);
    }
    return result;
}

HookExecResult HookEngine::execute_hook(const Hook& hook, SystemEvent event) {
    HookExecResult result;
    result.hook_id = hook.id;
    result.event = event;
    result.timed_out = false;

    auto start = std::chrono::steady_clock::now();

    // Create pipes for stdout/stderr
    int stdout_pipe[2];
    int stderr_pipe[2];
    if (pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) {
        result.exit_code = -1;
        result.stderr_output = "Failed to create pipes";
        result.duration = std::chrono::milliseconds(0);
        return result;
    }

    pid_t pid = fork();
    if (pid < 0) {
        result.exit_code = -1;
        result.stderr_output = "Fork failed";
        result.duration = std::chrono::milliseconds(0);
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        close(stderr_pipe[0]); close(stderr_pipe[1]);
        return result;
    }

    if (pid == 0) {
        // Child process
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stdout_pipe[1]);
        close(stderr_pipe[1]);

        // Set environment
        setenv("SL_EVENT", event_name(event).c_str(), 1);
        setenv("SL_HOOK_ID", hook.id.c_str(), 1);

        execl("/bin/sh", "sh", "-c", hook.script_path.c_str(), nullptr);
        _exit(127);
    }

    // Parent process
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    // Read output with timeout
    auto deadline = start + std::chrono::seconds(hook.timeout_seconds);

    std::array<char, 4096> buf;
    ssize_t n;

    // Read stdout
    while ((n = read(stdout_pipe[0], buf.data(), buf.size())) > 0) {
        result.stdout_output.append(buf.data(), static_cast<size_t>(n));
        if (std::chrono::steady_clock::now() > deadline) break;
    }
    close(stdout_pipe[0]);

    // Read stderr
    while ((n = read(stderr_pipe[0], buf.data(), buf.size())) > 0) {
        result.stderr_output.append(buf.data(), static_cast<size_t>(n));
        if (std::chrono::steady_clock::now() > deadline) break;
    }
    close(stderr_pipe[0]);

    // Wait for child
    int status = 0;
    if (std::chrono::steady_clock::now() > deadline) {
        // Timeout — kill the child
        kill(pid, SIGTERM);
        usleep(100000); // 100ms grace
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        result.timed_out = true;
        result.exit_code = -1;
    } else {
        waitpid(pid, &status, 0);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }

    auto end = std::chrono::steady_clock::now();
    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    return result;
}

std::vector<HookExecResult> HookEngine::fire(SystemEvent event) {
    std::lock_guard lock(mutex_);
    std::vector<HookExecResult> results;

    SL_INFO("hooks: firing event '{}'", event_name(event));

    for (const auto& hook : hooks_) {
        if (hook.event != event || !hook.enabled) continue;

        SL_INFO("hooks: executing '{}' for event '{}'", hook.id, event_name(event));
        auto result = execute_hook(hook, event);

        // Record history
        HookHistoryEntry entry;
        entry.timestamp = timestamp_now();
        entry.hook_id = hook.id;
        entry.event = event;
        entry.exit_code = result.exit_code;
        entry.timed_out = result.timed_out;
        entry.duration = result.duration;
        history_.push_back(std::move(entry));

        if (history_.size() > 1000) {
            history_.erase(history_.begin());
        }

        if (result.exit_code != 0) {
            SL_WARN("hooks: '{}' exited with code {} (timed_out={})",
                    hook.id, result.exit_code, result.timed_out);
        }

        results.push_back(std::move(result));
    }

    return results;
}

std::vector<HookExecResult> HookEngine::test_fire(SystemEvent event) {
    // Same as fire but logged as test
    SL_INFO("hooks: TEST firing event '{}'", event_name(event));
    return fire(event);
}

std::vector<HookHistoryEntry> HookEngine::get_history(int last_n) const {
    std::lock_guard lock(mutex_);
    if (static_cast<int>(history_.size()) <= last_n) {
        return history_;
    }
    return std::vector<HookHistoryEntry>(
        history_.end() - last_n, history_.end());
}

} // namespace straylight
