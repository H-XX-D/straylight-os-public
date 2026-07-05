// tools/echo/ipc_client.h
// IPC client for snapshot, rewind, and replay services.
#pragma once

#include <straylight/result.h>
#include <straylight/ipc_client.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace straylight {

/// Client for the snapshot service — filesystem state capture.
class SnapshotClient {
public:
    /// Create a new filesystem snapshot. Returns the snapshot ID.
    Result<std::string, std::string> snapshot_create(const std::string& name = "");

    /// Restore filesystem to a snapshot by ID.
    Result<void, std::string> snapshot_restore(const std::string& snapshot_id);

    /// Delete a snapshot by ID.
    Result<void, std::string> snapshot_delete(const std::string& snapshot_id);

    /// List available snapshots. Returns JSON array of snapshot info.
    Result<nlohmann::json, std::string> snapshot_list();

    /// Get info about a specific snapshot.
    Result<nlohmann::json, std::string> snapshot_info(const std::string& snapshot_id);

private:
    Result<nlohmann::json, std::string> call(const std::string& method,
                                              const nlohmann::json& params = {});
    static constexpr const char* SOCKET = "/run/straylight/snapshot.sock";
};

/// Client for the rewind service — process checkpoint/restore.
class RewindClient {
public:
    /// Checkpoint a process. Returns the checkpoint ID.
    Result<std::string, std::string> rewind_checkpoint(pid_t pid);

    /// Restore a process from a checkpoint.
    Result<void, std::string> rewind_restore(pid_t pid, const std::string& checkpoint_id);

    /// List checkpoints for a process.
    Result<nlohmann::json, std::string> rewind_list(pid_t pid);

    /// Delete a checkpoint.
    Result<void, std::string> rewind_delete(const std::string& checkpoint_id);

private:
    Result<nlohmann::json, std::string> call(const std::string& method,
                                              const nlohmann::json& params = {});
    static constexpr const char* SOCKET = "/run/straylight/rewind.sock";
};

/// Client for the replay service — ring buffer position management.
class ReplayClient {
public:
    /// Get current replay ring buffer position.
    Result<uint64_t, std::string> replay_get_position();

    /// Seek the replay ring buffer to a position.
    Result<void, std::string> replay_seek(uint64_t position);

    /// Get the current replay buffer size.
    Result<uint64_t, std::string> replay_get_size();

private:
    Result<nlohmann::json, std::string> call(const std::string& method,
                                              const nlohmann::json& params = {});
    static constexpr const char* SOCKET = "/run/straylight/replay.sock";
};

} // namespace straylight
