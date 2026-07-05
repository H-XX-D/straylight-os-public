// tools/update/update_manager.h
// System update management for StrayLight OS — apt integration with snapshots.
#pragma once

#include <straylight/result.h>

#include <chrono>
#include <string>
#include <vector>

namespace straylight {

/// Represents an available package update.
struct PackageUpdate {
    std::string name;
    std::string current_version;
    std::string new_version;
    std::string section;        // "security", "stable", "backports"
    uint64_t download_size = 0;
    bool is_security = false;
    std::string changelog;
};

/// Represents a completed update operation.
struct UpdateRecord {
    std::string id;
    std::chrono::system_clock::time_point timestamp;
    std::string snapshot_name;
    std::vector<std::string> packages_upgraded;
    std::vector<std::string> packages_installed;
    std::vector<std::string> packages_removed;
    bool success = false;
    std::string error_message;
};

/// Package hold status.
struct PackageHold {
    std::string name;
    std::string version;
    std::string held_since;
};

class UpdateManager {
public:
    UpdateManager();
    ~UpdateManager();

    /// Check for available updates (runs apt update + apt list --upgradable).
    Result<std::vector<PackageUpdate>, std::string> check() const;

    /// Perform system upgrade with optional pre-flight snapshot.
    Result<UpdateRecord, std::string> upgrade(bool auto_snapshot = true,
                                                bool security_only = false,
                                                bool dry_run = false);

    /// Rollback to the pre-update snapshot.
    Result<void, std::string> rollback() const;

    /// Get update history.
    Result<std::vector<UpdateRecord>, std::string> history() const;

    /// Schedule automatic updates via straylight-cron.
    Result<void, std::string> schedule(const std::string& cron_expr);

    /// Remove scheduled updates.
    Result<void, std::string> unschedule();

    /// Get current schedule if any.
    Result<std::string, std::string> get_schedule() const;

    /// Hold a package (prevent it from being upgraded).
    Result<void, std::string> hold(const std::string& package);

    /// Unhold a package.
    Result<void, std::string> unhold(const std::string& package);

    /// List held packages.
    Result<std::vector<PackageHold>, std::string> list_holds() const;

    /// Get changelog for a specific package.
    Result<std::string, std::string> changelog(const std::string& package) const;

    /// Clean apt cache.
    Result<void, std::string> clean() const;

    /// Show disk space that would be freed by cleaning.
    Result<uint64_t, std::string> clean_estimate() const;

private:
    static constexpr const char* kHistoryFile = "/var/lib/straylight/updates/history.json";
    static constexpr const char* kScheduleFile = "/var/lib/straylight/updates/schedule.json";

    /// Run a shell command and capture output.
    Result<std::string, std::string> run_cmd(const std::string& cmd) const;

    /// Write an update record to history.
    Result<void, std::string> write_history(const UpdateRecord& record) const;

    /// Generate a unique update ID.
    std::string generate_id() const;

    /// Create a pre-update snapshot via straylight-snapshot.
    Result<std::string, std::string> create_snapshot(const std::string& label) const;

    /// Restore a snapshot via straylight-snapshot.
    Result<void, std::string> restore_snapshot(const std::string& name) const;

    /// Parse apt list --upgradable output.
    std::vector<PackageUpdate> parse_upgradable(const std::string& output) const;

    /// Ensure history directory exists.
    void ensure_dirs() const;

    /// Get the name of the most recent pre-update snapshot.
    Result<std::string, std::string> last_snapshot() const;
};

} // namespace straylight
