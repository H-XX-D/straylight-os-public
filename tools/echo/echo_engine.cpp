// tools/echo/echo_engine.cpp
#include "echo_engine.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>

namespace straylight {

EchoEngine::EchoEngine() {
    load_states();
}

void EchoEngine::load_states() {
    states_.clear();

    if (!std::filesystem::exists(STATE_FILE)) return;

    std::ifstream ifs(STATE_FILE);
    if (!ifs.is_open()) return;

    nlohmann::json j;
    try {
        ifs >> j;
    } catch (...) {
        return;
    }

    if (!j.contains("states") || !j["states"].is_array()) return;

    for (const auto& entry : j["states"]) {
        SystemState state;
        state.state_id = entry.value("state_id", "");
        state.snapshot_id = entry.value("snapshot_id", "");
        state.replay_position = entry.value("replay_position", static_cast<uint64_t>(0));
        state.description = entry.value("description", "");
        state.estimated_size_bytes = entry.value("estimated_size_bytes", static_cast<uint64_t>(0));

        int64_t epoch_ms = entry.value("timestamp_ms", static_cast<int64_t>(0));
        state.timestamp = std::chrono::system_clock::time_point(
            std::chrono::milliseconds(epoch_ms));

        if (entry.contains("checkpoints") && entry["checkpoints"].is_array()) {
            for (const auto& cp : entry["checkpoints"]) {
                pid_t pid = cp.value("pid", 0);
                std::string cp_id = cp.value("checkpoint_id", "");
                if (pid > 0 && !cp_id.empty()) {
                    state.checkpoint_ids.emplace_back(pid, cp_id);
                }
            }
        }

        states_.push_back(std::move(state));
    }

    // Load tracked PIDs
    if (j.contains("tracked_pids") && j["tracked_pids"].is_array()) {
        for (const auto& pid : j["tracked_pids"]) {
            tracked_pids_.push_back(pid.get<pid_t>());
        }
    }

    auto_save_interval_ = j.value("auto_save_interval", 0);
}

void EchoEngine::save_states() const {
    auto dir = std::filesystem::path(STATE_FILE).parent_path();
    std::filesystem::create_directories(dir);

    nlohmann::json j;
    nlohmann::json states_arr = nlohmann::json::array();

    for (const auto& state : states_) {
        nlohmann::json entry;
        entry["state_id"] = state.state_id;
        entry["snapshot_id"] = state.snapshot_id;
        entry["replay_position"] = state.replay_position;
        entry["description"] = state.description;
        entry["estimated_size_bytes"] = state.estimated_size_bytes;

        auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            state.timestamp.time_since_epoch()).count();
        entry["timestamp_ms"] = epoch_ms;

        nlohmann::json checkpoints = nlohmann::json::array();
        for (const auto& [pid, cp_id] : state.checkpoint_ids) {
            nlohmann::json cp;
            cp["pid"] = pid;
            cp["checkpoint_id"] = cp_id;
            checkpoints.push_back(cp);
        }
        entry["checkpoints"] = checkpoints;

        states_arr.push_back(entry);
    }

    j["states"] = states_arr;
    j["tracked_pids"] = tracked_pids_;
    j["auto_save_interval"] = auto_save_interval_;

    std::ofstream ofs(STATE_FILE);
    ofs << j.dump(2);
}

std::string EchoEngine::generate_state_id() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);

    auto now = std::chrono::system_clock::now();
    auto epoch = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();

    char buf[32];
    std::snprintf(buf, sizeof(buf), "echo-%lx-%08x",
                  static_cast<unsigned long>(epoch), dist(gen));
    return std::string(buf);
}

uint64_t EchoEngine::estimate_state_size(const SystemState& state) {
    uint64_t size = 0;
    // Each snapshot is roughly tracked by the snapshot service
    // Estimate: 1 MB for metadata, actual data is CoW or compressed
    size += 1024 * 1024;
    // Each checkpoint is typically 10-100 MB
    size += state.checkpoint_ids.size() * 50 * 1024 * 1024;
    // Replay position is just an offset — negligible
    size += 64;
    return size;
}

Result<const SystemState*, std::string> EchoEngine::find_state_before(
    std::chrono::system_clock::time_point target) const {
    const SystemState* best = nullptr;

    for (const auto& state : states_) {
        if (state.timestamp <= target) {
            if (!best || state.timestamp > best->timestamp) {
                best = &state;
            }
        }
    }

    if (!best) {
        return Result<const SystemState*, std::string>::error(
            "No saved state found before the target time");
    }

    return Result<const SystemState*, std::string>::ok(best);
}

Result<SystemState, std::string> EchoEngine::save_state(const std::string& description) {
    SystemState state;
    state.state_id = generate_state_id();
    state.timestamp = std::chrono::system_clock::now();
    state.description = description;

    std::cout << "echo: saving state " << state.state_id << "\n";

    // 1. Take filesystem snapshot
    std::cout << "echo: creating filesystem snapshot...\n";
    auto snap_result = snapshot_client_.snapshot_create("echo-" + state.state_id);
    if (snap_result.has_value()) {
        state.snapshot_id = snap_result.value();
        std::cout << "echo: snapshot " << state.snapshot_id << "\n";
    } else {
        std::cerr << "echo: warning: filesystem snapshot failed: " << snap_result.error() << "\n";
    }

    // 2. Checkpoint tracked processes
    for (pid_t pid : tracked_pids_) {
        std::cout << "echo: checkpointing process " << pid << "...\n";
        auto cp_result = rewind_client_.rewind_checkpoint(pid);
        if (cp_result.has_value()) {
            state.checkpoint_ids.emplace_back(pid, cp_result.value());
            std::cout << "echo: checkpoint " << cp_result.value() << " for pid " << pid << "\n";
        } else {
            std::cerr << "echo: warning: checkpoint failed for pid " << pid
                      << ": " << cp_result.error() << "\n";
        }
    }

    // 3. Record replay ring buffer position
    auto replay_result = replay_client_.replay_get_position();
    if (replay_result.has_value()) {
        state.replay_position = replay_result.value();
        std::cout << "echo: replay position " << state.replay_position << "\n";
    } else {
        state.replay_position = 0;
        std::cerr << "echo: warning: replay position unavailable: "
                  << replay_result.error() << "\n";
    }

    state.estimated_size_bytes = estimate_state_size(state);

    // Store and persist
    states_.push_back(state);

    // Enforce max states limit
    while (states_.size() > MAX_STATES) {
        // Remove the oldest state (but keep the most recent ones)
        auto oldest = states_.begin();
        // Try to clean up the snapshot
        if (!oldest->snapshot_id.empty()) {
            snapshot_client_.snapshot_delete(oldest->snapshot_id);
        }
        for (const auto& [pid, cp_id] : oldest->checkpoint_ids) {
            rewind_client_.rewind_delete(cp_id);
        }
        states_.erase(oldest);
    }

    save_states();

    std::cout << "echo: state saved (" << state.checkpoint_ids.size()
              << " processes, ~" << state.estimated_size_bytes / (1024 * 1024)
              << " MB estimated)\n";

    return Result<SystemState, std::string>::ok(std::move(state));
}

Result<UndoReport, std::string> EchoEngine::undo(int seconds_ago) {
    auto target = std::chrono::system_clock::now() - std::chrono::seconds(seconds_ago);

    auto state_result = find_state_before(target);
    if (!state_result.has_value()) {
        return Result<UndoReport, std::string>::error(state_result.error());
    }

    const auto* state = state_result.value();

    std::cout << "echo: undoing to state " << state->state_id
              << " (" << seconds_ago << "s ago)\n";

    // Save current state for redo before we undo
    auto pre_undo = save_state("pre-undo");
    if (pre_undo.has_value()) {
        pre_undo_state_ = std::make_unique<SystemState>(std::move(pre_undo).value());
    }

    UndoReport report;
    report.state_id = state->state_id;
    report.filesystem_restored = false;
    report.processes_restored = 0;
    report.replay_seeked = false;

    std::ostringstream details;

    // 1. Restore filesystem snapshot
    if (!state->snapshot_id.empty()) {
        std::cout << "echo: restoring filesystem snapshot " << state->snapshot_id << "...\n";
        auto result = snapshot_client_.snapshot_restore(state->snapshot_id);
        if (result.has_value()) {
            report.filesystem_restored = true;
            details << "Filesystem restored from snapshot " << state->snapshot_id << "\n";
        } else {
            details << "Filesystem restore failed: " << result.error() << "\n";
        }
    }

    // 2. Restore process checkpoints
    for (const auto& [pid, cp_id] : state->checkpoint_ids) {
        std::cout << "echo: restoring process " << pid << " from checkpoint " << cp_id << "...\n";
        auto result = rewind_client_.rewind_restore(pid, cp_id);
        if (result.has_value()) {
            report.processes_restored++;
            details << "Process " << pid << " restored from checkpoint " << cp_id << "\n";
        } else {
            details << "Process " << pid << " restore failed: " << result.error() << "\n";
        }
    }

    // 3. Seek replay buffer
    if (state->replay_position > 0) {
        std::cout << "echo: seeking replay to position " << state->replay_position << "...\n";
        auto result = replay_client_.replay_seek(state->replay_position);
        if (result.has_value()) {
            report.replay_seeked = true;
            details << "Replay buffer seeked to position " << state->replay_position << "\n";
        } else {
            details << "Replay seek failed: " << result.error() << "\n";
        }
    }

    report.details = details.str();

    std::cout << "echo: undo complete — "
              << (report.filesystem_restored ? "fs:yes" : "fs:no") << " "
              << "procs:" << report.processes_restored << " "
              << (report.replay_seeked ? "replay:yes" : "replay:no") << "\n";

    return Result<UndoReport, std::string>::ok(std::move(report));
}

Result<UndoReport, std::string> EchoEngine::undo_selective(
    const std::string& component, int seconds_ago) {
    auto target = std::chrono::system_clock::now() - std::chrono::seconds(seconds_ago);

    auto state_result = find_state_before(target);
    if (!state_result.has_value()) {
        return Result<UndoReport, std::string>::error(state_result.error());
    }

    const auto* state = state_result.value();

    UndoReport report;
    report.state_id = state->state_id;
    report.filesystem_restored = false;
    report.processes_restored = 0;
    report.replay_seeked = false;

    std::ostringstream details;

    if (component == "filesystem") {
        if (!state->snapshot_id.empty()) {
            auto result = snapshot_client_.snapshot_restore(state->snapshot_id);
            if (result.has_value()) {
                report.filesystem_restored = true;
                details << "Filesystem restored from snapshot " << state->snapshot_id << "\n";
            } else {
                details << "Filesystem restore failed: " << result.error() << "\n";
            }
        } else {
            details << "No filesystem snapshot in this state\n";
        }
    } else if (component == "replay") {
        if (state->replay_position > 0) {
            auto result = replay_client_.replay_seek(state->replay_position);
            if (result.has_value()) {
                report.replay_seeked = true;
                details << "Replay seeked to " << state->replay_position << "\n";
            } else {
                details << "Replay seek failed: " << result.error() << "\n";
            }
        }
    } else if (component.rfind("process:", 0) == 0) {
        // Parse PID from "process:<pid>"
        pid_t target_pid = static_cast<pid_t>(std::stoi(component.substr(8)));

        for (const auto& [pid, cp_id] : state->checkpoint_ids) {
            if (pid == target_pid) {
                auto result = rewind_client_.rewind_restore(pid, cp_id);
                if (result.has_value()) {
                    report.processes_restored++;
                    details << "Process " << pid << " restored from " << cp_id << "\n";
                } else {
                    details << "Process " << pid << " restore failed: " << result.error() << "\n";
                }
                break;
            }
        }
        if (report.processes_restored == 0) {
            details << "No checkpoint found for PID " << target_pid << " in this state\n";
        }
    } else {
        return Result<UndoReport, std::string>::error(
            "Unknown component: " + component +
            " (expected 'filesystem', 'replay', or 'process:<pid>')");
    }

    report.details = details.str();
    return Result<UndoReport, std::string>::ok(std::move(report));
}

Result<UndoReport, std::string> EchoEngine::redo() {
    if (!pre_undo_state_) {
        return Result<UndoReport, std::string>::error("No undo to redo — no pre-undo state saved");
    }

    const auto& state = *pre_undo_state_;

    std::cout << "echo: redo — restoring pre-undo state " << state.state_id << "\n";

    UndoReport report;
    report.state_id = state.state_id;
    report.filesystem_restored = false;
    report.processes_restored = 0;
    report.replay_seeked = false;

    std::ostringstream details;

    // Restore filesystem
    if (!state.snapshot_id.empty()) {
        auto result = snapshot_client_.snapshot_restore(state.snapshot_id);
        if (result.has_value()) {
            report.filesystem_restored = true;
            details << "Filesystem restored to pre-undo snapshot\n";
        } else {
            details << "Filesystem redo failed: " << result.error() << "\n";
        }
    }

    // Restore processes
    for (const auto& [pid, cp_id] : state.checkpoint_ids) {
        auto result = rewind_client_.rewind_restore(pid, cp_id);
        if (result.has_value()) {
            report.processes_restored++;
            details << "Process " << pid << " restored to pre-undo state\n";
        } else {
            details << "Process " << pid << " redo failed: " << result.error() << "\n";
        }
    }

    // Seek replay
    if (state.replay_position > 0) {
        auto result = replay_client_.replay_seek(state.replay_position);
        if (result.has_value()) {
            report.replay_seeked = true;
            details << "Replay buffer restored to pre-undo position\n";
        }
    }

    report.details = details.str();

    // Clear pre-undo state after redo
    pre_undo_state_.reset();

    return Result<UndoReport, std::string>::ok(std::move(report));
}

std::vector<SystemState> EchoEngine::list_states() const {
    return states_;
}

void EchoEngine::track_process(pid_t pid) {
    if (std::find(tracked_pids_.begin(), tracked_pids_.end(), pid) == tracked_pids_.end()) {
        tracked_pids_.push_back(pid);
        save_states();
    }
}

void EchoEngine::untrack_process(pid_t pid) {
    tracked_pids_.erase(
        std::remove(tracked_pids_.begin(), tracked_pids_.end(), pid),
        tracked_pids_.end());
    save_states();
}

} // namespace straylight
