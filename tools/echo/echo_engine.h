// tools/echo/echo_engine.h
// Unified system-wide undo — orchestrates snapshot, rewind, and replay services.
#pragma once

#include "ipc_client.h"

#include <straylight/result.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace straylight {

/// A unified system state captured across all subsystems.
struct SystemState {
    std::string state_id;
    std::chrono::system_clock::time_point timestamp;
    std::string snapshot_id;                                   // Filesystem snapshot
    std::vector<std::pair<pid_t, std::string>> checkpoint_ids; // Per-process checkpoints
    uint64_t replay_position;                                  // Replay ring buffer position
    std::string description;
    uint64_t estimated_size_bytes;
};

/// What was undone by an undo operation.
struct UndoReport {
    std::string state_id;
    bool filesystem_restored;
    int processes_restored;
    bool replay_seeked;
    std::string details;
};

/// Orchestrates undo across snapshot, rewind, and replay services.
class EchoEngine {
public:
    EchoEngine();

    /// Save the current state across all subsystems.
    Result<SystemState, std::string> save_state(const std::string& description = "");

    /// Undo to a state N seconds ago.
    Result<UndoReport, std::string> undo(int seconds_ago);

    /// Undo only a specific component: "filesystem", "process:<pid>", or "replay".
    Result<UndoReport, std::string> undo_selective(const std::string& component,
                                                    int seconds_ago);

    /// Redo the last undo (restore the pre-undo state).
    Result<UndoReport, std::string> redo();

    /// List all saved states.
    std::vector<SystemState> list_states() const;

    /// Get auto-save interval (0 = disabled).
    int auto_save_interval_seconds() const { return auto_save_interval_; }

    /// Set auto-save interval.
    void set_auto_save_interval(int seconds) { auto_save_interval_ = seconds; }

    /// Get tracked process PIDs for checkpoint operations.
    std::vector<pid_t> tracked_processes() const { return tracked_pids_; }

    /// Add a process to track for undo checkpoints.
    void track_process(pid_t pid);

    /// Remove a process from tracking.
    void untrack_process(pid_t pid);

private:
    /// Load states from the persistent state file.
    void load_states();

    /// Save states to the persistent state file.
    void save_states() const;

    /// Find the nearest state before a target time.
    Result<const SystemState*, std::string> find_state_before(
        std::chrono::system_clock::time_point target) const;

    /// Generate a unique state ID.
    static std::string generate_state_id();

    /// Estimate the total size of a state across subsystems.
    static uint64_t estimate_state_size(const SystemState& state);

    SnapshotClient snapshot_client_;
    RewindClient rewind_client_;
    ReplayClient replay_client_;

    std::vector<SystemState> states_;
    std::vector<pid_t> tracked_pids_;

    /// Pre-undo state for redo support.
    std::unique_ptr<SystemState> pre_undo_state_;

    int auto_save_interval_ = 0;

    static constexpr const char* STATE_FILE = "/var/lib/straylight/echo/states.json";
    static constexpr size_t MAX_STATES = 256;
};

} // namespace straylight
