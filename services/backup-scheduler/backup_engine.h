// services/backup-scheduler/backup_engine.h
// Automated backup scheduler — snapshots + rsync with encryption.
#pragma once

#include <straylight/result.h>
#include <straylight/error.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace straylight {

/// Retention policy for backups.
struct RetentionPolicy {
    int keep_daily = 7;
    int keep_weekly = 4;
    int keep_monthly = 12;
};

/// Remote backup target for rsync.
struct RemoteTarget {
    std::string name;
    std::string host;
    std::string path;
    std::string ssh_key;
    int port = 22;
    int bandwidth_limit_kbps = 0; // 0 = unlimited
};

/// A completed backup record.
struct BackupRecord {
    std::string id;
    std::string type;  // "snapshot" or "rsync"
    std::string source;
    std::string destination;
    std::chrono::system_clock::time_point timestamp;
    uint64_t size_bytes = 0;
    bool encrypted = false;
    bool verified = false;
    std::string status; // "ok", "failed", "partial"
    std::string error_message;
};

/// Schedule entry.
struct BackupSchedule {
    std::string name;
    std::string source;
    std::string cron_expression;  // Simplified: "daily", "weekly", "monthly", or cron
    bool enabled = false;
    bool encrypt = false;
    std::string gpg_recipient;
    std::vector<std::string> remote_targets;
    RetentionPolicy retention;
};

class BackupEngine {
public:
    BackupEngine() = default;

    /// Initialize from config.
    Result<void, SLError> init(const std::filesystem::path& config_path);

    /// Run a backup now for the given schedule (or all).
    Result<BackupRecord, SLError> run_backup(const std::string& schedule_name = "");

    /// Run a filesystem snapshot.
    Result<BackupRecord, SLError> snapshot(const std::string& source,
                                            bool encrypt = false,
                                            const std::string& gpg_recipient = "");

    /// Rsync to a remote target.
    Result<BackupRecord, SLError> rsync_to_remote(const std::string& source,
                                                    const RemoteTarget& target);

    /// List all backup records.
    std::vector<BackupRecord> list_backups() const;

    /// Restore a backup by ID.
    Result<void, SLError> restore(const std::string& backup_id,
                                   const std::string& target_path = "");

    /// Verify a backup by ID.
    Result<bool, SLError> verify(const std::string& backup_id);

    /// Add a remote target.
    Result<void, SLError> add_remote(const RemoteTarget& target);

    /// Remove a remote target.
    Result<void, SLError> remove_remote(const std::string& name);

    /// List remote targets.
    std::vector<RemoteTarget> list_remotes() const;

    /// Get all schedules.
    std::vector<BackupSchedule> list_schedules() const;

    /// Apply retention policy — remove old backups.
    Result<int, SLError> apply_retention(const RetentionPolicy& policy);

    /// Check if a scheduled backup is due.
    bool is_due(const BackupSchedule& schedule) const;

    /// Tick — called periodically to check schedules.
    Result<void, SLError> tick();

private:
    /// Generate a unique backup ID.
    std::string generate_id() const;

    /// Save the backup database.
    void save_database() const;

    /// Load the backup database.
    void load_database();

    /// Encrypt a file with GPG.
    Result<std::filesystem::path, SLError> encrypt_file(
        const std::filesystem::path& file, const std::string& recipient);

    mutable std::mutex mutex_;
    std::filesystem::path backup_dir_ = "/var/lib/straylight/backups";
    std::filesystem::path db_path_ = "/var/lib/straylight/backups/backup.db.json";
    std::vector<BackupRecord> records_;
    std::vector<BackupSchedule> schedules_;
    std::map<std::string, RemoteTarget> remotes_;
    std::map<std::string, std::chrono::system_clock::time_point> last_run_;
};

} // namespace straylight
