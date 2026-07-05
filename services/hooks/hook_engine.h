// services/hooks/hook_engine.h
// System event hooks — trigger scripts on system events.
#pragma once

#include <straylight/result.h>
#include <straylight/error.h>

#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace straylight {

/// System events that can trigger hooks.
enum class SystemEvent {
    Boot,
    Shutdown,
    Suspend,
    Resume,
    NetworkUp,
    NetworkDown,
    UsbAttach,
    UsbDetach,
    LidOpen,
    LidClose,
    BatteryLow,
    PowerAC,
    PowerBattery,
};

/// A registered hook.
struct Hook {
    std::string id;
    SystemEvent event;
    std::string script_path;
    int timeout_seconds = 30;
    bool enabled = true;
    int priority = 50; // 0-100, lower runs first
};

/// Execution result for a hook.
struct HookExecResult {
    std::string hook_id;
    SystemEvent event;
    int exit_code;
    std::string stdout_output;
    std::string stderr_output;
    std::chrono::milliseconds duration;
    bool timed_out;
};

/// History entry.
struct HookHistoryEntry {
    std::string timestamp;
    std::string hook_id;
    SystemEvent event;
    int exit_code;
    bool timed_out;
    std::chrono::milliseconds duration;
};

class HookEngine {
public:
    HookEngine() = default;

    /// Load hooks from a hooks directory.
    Result<void, SLError> load_hooks(const std::filesystem::path& hooks_dir);

    /// Register a new hook.
    Result<void, SLError> add_hook(const Hook& hook);

    /// Remove a hook by ID.
    Result<void, SLError> remove_hook(const std::string& id);

    /// List all hooks, optionally filtered by event.
    std::vector<Hook> list_hooks(SystemEvent event = static_cast<SystemEvent>(-1)) const;

    /// Fire all hooks for an event.
    std::vector<HookExecResult> fire(SystemEvent event);

    /// Test fire a specific event (dry-run or real execution).
    std::vector<HookExecResult> test_fire(SystemEvent event);

    /// Get execution history.
    std::vector<HookHistoryEntry> get_history(int last_n = 50) const;

    /// Convert event name to enum.
    static Result<SystemEvent, std::string> parse_event(const std::string& name);

    /// Convert event enum to name.
    static std::string event_name(SystemEvent event);

    /// Generate a unique hook ID.
    static std::string generate_id();

private:
    /// Execute a single hook script with timeout.
    HookExecResult execute_hook(const Hook& hook, SystemEvent event);

    mutable std::mutex mutex_;
    std::vector<Hook> hooks_;
    std::vector<HookHistoryEntry> history_;
    std::filesystem::path hooks_dir_;
};

} // namespace straylight
