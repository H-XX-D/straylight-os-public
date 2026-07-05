// tools/migrate/migrator.h
// System migration: export, import, sync, and diff for StrayLight OS.
#pragma once

#include <straylight/result.h>
#include "diff_engine.h"

#include <functional>
#include <string>
#include <vector>

namespace straylight {

/// Progress callback for long-running operations.
using MigrateProgressFn = std::function<void(const std::string& phase, int percent)>;

/// System migration manager.
/// Exports and imports StrayLight system configuration archives.
class Migrator {
public:
    /// Set progress callback.
    void set_progress(MigrateProgressFn fn) { progress_ = std::move(fn); }

    /// Export the local system configuration to a compressed archive.
    /// Creates a tar.zst file containing:
    ///   - /etc/straylight/* (system config)
    ///   - ~/.config/straylight/* (user config)
    ///   - Package list (dpkg/rpm/brew)
    ///   - systemd unit states
    ///   - SSH keys (if include_ssh is true)
    ///   - manifest.json (for differential sync)
    Result<uint64_t, std::string> export_archive(
        const std::string& output_path,
        bool include_ssh = false);

    /// Import a migration archive onto the local system.
    /// Extracts and applies configuration from a tar.zst archive.
    Result<int, std::string> import_archive(
        const std::string& archive_path,
        bool dry_run = false);

    /// Live sync to a remote StrayLight machine via SSH.
    /// Uses rsync over SSH for efficient differential transfer.
    Result<void, std::string> sync_to_remote(
        const std::string& host,
        const std::string& user = "root");

    /// Show differences between local and remote machine.
    Result<DiffSummary, std::string> diff_remote(
        const std::string& host,
        const std::string& user = "root");

private:
    /// Run a shell command and capture output.
    struct CmdResult {
        int exit_code = -1;
        std::string stdout_str;
        std::string stderr_str;
    };
    static CmdResult run_command(const std::string& cmd);

    /// Build tar command for StrayLight config paths.
    static std::string build_tar_paths();

    /// Ensure zstd is available.
    static bool has_zstd();

    /// Report progress.
    void report(const std::string& phase, int percent);

    MigrateProgressFn progress_;
    DiffEngine diff_engine_;
};

} // namespace straylight
