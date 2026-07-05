// tools/snapshot/snapshot_manager.h
// Manages btrfs-based (or rsync fallback) system snapshots for StrayLight OS.
#pragma once

#include <straylight/result.h>

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace straylight {

class SnapshotManager {
public:
    struct Snapshot {
        std::string name;
        std::string description;
        std::chrono::system_clock::time_point created;
        std::string btrfs_path;      // /.snapshots/<name>
        std::string service_state;   // JSON of systemd unit states
        size_t size_bytes = 0;
        bool is_auto = false;
    };

    SnapshotManager();
    ~SnapshotManager();

    /// Create a named snapshot of the running system.
    Result<Snapshot, std::string> save(const std::string& name,
                                       const std::string& desc = "");

    /// Restore the system to a previously saved snapshot.
    Result<void, std::string> restore(const std::string& name);

    /// Undo the last restore by reverting to the pre-restore snapshot.
    Result<void, std::string> rollback();

    /// List every snapshot on disk, sorted newest-first.
    std::vector<Snapshot> list() const;

    /// Show a textual diff of what changed since <name>.
    Result<std::string, std::string> diff(const std::string& name);

    /// Delete a single snapshot and its metadata.
    Result<void, std::string> remove(const std::string& name);

    /// Install a systemd timer that creates automatic snapshots.
    Result<void, std::string> auto_enable(int interval_secs, int keep_count);

private:
    static constexpr const char* kSnapshotDir = "/var/lib/straylight/snapshots";
    static constexpr const char* kSandboxBase = "/var/lib/straylight/sandboxes";

    bool is_btrfs() const;
    Result<void, std::string> ensure_snapshot_dir() const;
    std::string meta_path(const std::string& name) const;
    Result<std::string, std::string> run_cmd(const std::string& cmd) const;
    std::string capture_service_state() const;
    Result<Snapshot, std::string> load_meta(const std::string& name) const;
    void write_meta(const Snapshot& snap) const;
    size_t dir_size(const std::string& path) const;
};

} // namespace straylight
