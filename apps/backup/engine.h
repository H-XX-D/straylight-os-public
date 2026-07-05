// apps/backup/engine.h
// Incremental backup engine — rsync wrapper with snapshot directories
#pragma once

#include <straylight/result.h>
#include <straylight/error.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace straylight::backup {

namespace fs = std::filesystem;

/// Configuration for one backup task.
struct BackupProfile {
    std::string name;
    fs::path source;
    fs::path destination;
    std::vector<std::string> excludes;
    bool compress        = true;
    bool delete_removed  = false;
};

/// Record of a single completed (or failed) backup run.
struct BackupRun {
    std::chrono::system_clock::time_point timestamp;
    uint64_t files   = 0;
    uint64_t bytes   = 0;
    bool success     = false;
    std::string error_msg;
    std::string snapshot_dir; // absolute path of created snapshot
};

/// Progress callback: percentage (0-100), bytes transferred, current file name.
using ProgressFn = std::function<void(int pct, uint64_t bytes, const std::string& file)>;

/// Executes rsync-based incremental backups.
class Engine {
public:
    /// Run a backup for the given profile. Returns the BackupRun record on success.
    Result<BackupRun, SLError> run_backup(const BackupProfile& p,
                                           ProgressFn prog = {});

    /// Return the manifest (list of past runs) for the given profile.
    Result<std::vector<BackupRun>, SLError> history(const BackupProfile& p) const;

    /// Restore the snapshot closest to `snapshot` time to `to`.
    Result<void, SLError> restore(const BackupProfile& p,
                                   const fs::path& to,
                                   std::chrono::system_clock::time_point snapshot);

private:
    /// Build the rsync argument list for a run.
    std::vector<std::string> build_args(const BackupProfile& p,
                                         const fs::path& link_dest,
                                         const fs::path& snapshot_dir) const;

    /// Fork+exec rsync, read its stdout, parse progress output.
    Result<BackupRun, SLError> exec_rsync(const std::vector<std::string>& args,
                                           ProgressFn prog);

    /// Append a BackupRun to the profile's manifest.json.
    void write_manifest(const BackupProfile& p, const BackupRun& run) const;

    /// Return path for the profile's manifest file.
    static fs::path manifest_path(const BackupProfile& p);

    /// Return the newest snapshot dir under p.destination (empty if none).
    static fs::path latest_snapshot(const BackupProfile& p);

    /// Build snapshot directory name from a timestamp.
    static std::string snapshot_name(std::chrono::system_clock::time_point tp);
};

} // namespace straylight::backup
