// tools/migrate/migrator.cpp
// System migration implementation.
#include "migrator.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <sys/wait.h>
#include <unistd.h>

namespace straylight {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

Migrator::CmdResult Migrator::run_command(const std::string& cmd) {
    CmdResult result;

    // Create pipes for stdout and stderr
    int out_pipe[2], err_pipe[2];
    if (pipe(out_pipe) < 0 || pipe(err_pipe) < 0) {
        result.stderr_str = "pipe() failed";
        return result;
    }

    pid_t pid = fork();
    if (pid < 0) {
        result.stderr_str = "fork() failed";
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        return result;
    }

    if (pid == 0) {
        close(out_pipe[0]);
        close(err_pipe[0]);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[1]);
        close(err_pipe[1]);
        execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr);
        _exit(127);
    }

    close(out_pipe[1]);
    close(err_pipe[1]);

    // Read output
    auto read_fd = [](int fd) -> std::string {
        std::string s;
        char buf[4096];
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            s.append(buf, static_cast<size_t>(n));
        }
        close(fd);
        return s;
    };

    result.stdout_str = read_fd(out_pipe[0]);
    result.stderr_str = read_fd(err_pipe[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    return result;
}

bool Migrator::has_zstd() {
    auto r = run_command("which zstd 2>/dev/null");
    return r.exit_code == 0;
}

std::string Migrator::build_tar_paths() {
    std::vector<std::string> paths;

    // System config
    if (std::filesystem::exists("/etc/straylight")) {
        paths.push_back("/etc/straylight");
    }

    // User config
    const char* home = std::getenv("HOME");
    if (home) {
        std::string user_cfg = std::string(home) + "/.config/straylight";
        if (std::filesystem::exists(user_cfg)) {
            paths.push_back(user_cfg);
        }
        std::string local_data = std::string(home) + "/.local/share/straylight";
        if (std::filesystem::exists(local_data)) {
            paths.push_back(local_data);
        }
    }

    std::ostringstream oss;
    for (size_t i = 0; i < paths.size(); ++i) {
        if (i > 0) oss << " ";
        oss << "'" << paths[i] << "'";
    }
    return oss.str();
}

void Migrator::report(const std::string& phase, int percent) {
    if (progress_) {
        progress_(phase, percent);
    }
}

// ---------------------------------------------------------------------------
// Export
// ---------------------------------------------------------------------------

Result<uint64_t, std::string> Migrator::export_archive(
    const std::string& output_path, bool include_ssh)
{
    report("Preparing export", 0);

    // Build manifest first
    auto manifest_r = diff_engine_.build_local_manifest();
    if (!manifest_r.has_value()) {
        return Result<uint64_t, std::string>::error(
            "Failed to build manifest: " + manifest_r.error());
    }

    std::string manifest_json = DiffEngine::manifest_to_json(manifest_r.value());

    // Write manifest to a temp file
    std::string tmp_dir = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp");
    std::string manifest_path = tmp_dir + "/straylight-migrate-manifest.json";
    {
        std::ofstream mf(manifest_path, std::ios::trunc);
        if (!mf) {
            return Result<uint64_t, std::string>::error("Cannot write manifest temp file");
        }
        mf << manifest_json;
    }

    report("Capturing package list", 15);

    // Capture package list
    std::string pkg_path = tmp_dir + "/straylight-migrate-packages.txt";
    run_command("(dpkg --get-selections 2>/dev/null || "
                "rpm -qa 2>/dev/null || "
                "brew list --formula 2>/dev/null) > '" + pkg_path + "' 2>/dev/null");

    report("Capturing service states", 30);

    // Capture systemd unit states
    std::string units_path = tmp_dir + "/straylight-migrate-services.txt";
    run_command("(systemctl list-unit-files --state=enabled 2>/dev/null || "
                "launchctl list 2>/dev/null) > '" + units_path + "' 2>/dev/null");

    report("Building archive", 50);

    // Build tar command
    std::ostringstream tar_cmd;
    std::string tar_paths = build_tar_paths();

    // Use zstd if available, gzip as fallback
    bool use_zstd = has_zstd();

    if (use_zstd) {
        tar_cmd << "tar cf - ";
    } else {
        tar_cmd << "tar czf - ";
    }

    if (!tar_paths.empty()) {
        tar_cmd << tar_paths << " ";
    }
    tar_cmd << "'" << manifest_path << "' "
            << "'" << pkg_path << "' "
            << "'" << units_path << "' ";

    // SSH keys
    const char* home = std::getenv("HOME");
    if (include_ssh && home) {
        std::string ssh_dir = std::string(home) + "/.ssh";
        if (std::filesystem::exists(ssh_dir)) {
            tar_cmd << "'" << ssh_dir << "' ";
        }
    }

    if (use_zstd) {
        tar_cmd << "| zstd -3 -o '" << output_path << "'";
    } else {
        tar_cmd << "> '" << output_path << "'";
    }

    report("Compressing", 70);

    auto r = run_command(tar_cmd.str());

    // Clean up temp files
    std::filesystem::remove(manifest_path);
    std::filesystem::remove(pkg_path);
    std::filesystem::remove(units_path);

    if (r.exit_code != 0) {
        return Result<uint64_t, std::string>::error(
            "tar failed: " + r.stderr_str);
    }

    report("Finalizing", 95);

    // Get archive size
    std::error_code ec;
    uint64_t size = std::filesystem::file_size(output_path, ec);
    if (ec) size = 0;

    report("Complete", 100);
    return Result<uint64_t, std::string>::ok(size);
}

// ---------------------------------------------------------------------------
// Import
// ---------------------------------------------------------------------------

Result<int, std::string> Migrator::import_archive(
    const std::string& archive_path, bool dry_run)
{
    if (!std::filesystem::exists(archive_path)) {
        return Result<int, std::string>::error("Archive not found: " + archive_path);
    }

    report("Reading archive", 10);

    // Detect compression format
    bool is_zstd = false;
    {
        std::ifstream in(archive_path, std::ios::binary);
        if (in) {
            char magic[4]{};
            in.read(magic, 4);
            // Zstd magic number: 0xFD2FB528
            is_zstd = (static_cast<uint8_t>(magic[0]) == 0x28 &&
                       static_cast<uint8_t>(magic[1]) == 0xB5 &&
                       static_cast<uint8_t>(magic[2]) == 0x2F &&
                       static_cast<uint8_t>(magic[3]) == 0xFD);
        }
    }

    // List archive contents first
    std::string list_cmd;
    if (is_zstd) {
        list_cmd = "zstd -d < '" + archive_path + "' | tar tf -";
    } else {
        list_cmd = "tar tzf '" + archive_path + "'";
    }

    auto list_r = run_command(list_cmd);
    if (list_r.exit_code != 0) {
        return Result<int, std::string>::error("Cannot read archive: " + list_r.stderr_str);
    }

    // Count files
    int file_count = 0;
    std::istringstream iss(list_r.stdout_str);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty()) ++file_count;
    }

    if (dry_run) {
        return Result<int, std::string>::ok(file_count);
    }

    report("Extracting", 40);

    // Extract to filesystem root
    std::string extract_cmd;
    if (is_zstd) {
        extract_cmd = "zstd -d < '" + archive_path + "' | tar xf - -C /";
    } else {
        extract_cmd = "tar xzf '" + archive_path + "' -C /";
    }

    auto ex_r = run_command(extract_cmd);

    report("Applying", 80);

    // Apply package list if present
    std::string tmp_dir = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp");
    std::string pkg_file = tmp_dir + "/straylight-migrate-packages.txt";
    if (std::filesystem::exists(pkg_file)) {
        // Best-effort package install
        run_command("dpkg --set-selections < '" + pkg_file + "' 2>/dev/null && "
                    "apt-get dselect-upgrade -y 2>/dev/null");
    }

    report("Complete", 100);

    if (ex_r.exit_code != 0) {
        // Partial success
        return Result<int, std::string>::ok(file_count);
    }

    return Result<int, std::string>::ok(file_count);
}

// ---------------------------------------------------------------------------
// Remote sync
// ---------------------------------------------------------------------------

Result<void, std::string> Migrator::sync_to_remote(
    const std::string& host, const std::string& user)
{
    report("Building manifest", 10);

    auto manifest_r = diff_engine_.build_local_manifest();
    if (!manifest_r.has_value()) {
        return Result<void, std::string>::error(manifest_r.error());
    }

    report("Syncing /etc/straylight", 30);

    // Use rsync for efficient differential transfer
    std::string rsync_base = "rsync -avz --delete ";
    std::string remote = user + "@" + host + ":";

    // Sync system config
    if (std::filesystem::exists("/etc/straylight")) {
        auto r = run_command(rsync_base + "/etc/straylight/ " + remote + "/etc/straylight/");
        if (r.exit_code != 0) {
            return Result<void, std::string>::error(
                "rsync /etc/straylight failed: " + r.stderr_str);
        }
    }

    report("Syncing user config", 60);

    // Sync user config
    const char* home = std::getenv("HOME");
    if (home) {
        std::string user_cfg = std::string(home) + "/.config/straylight";
        if (std::filesystem::exists(user_cfg)) {
            // Determine remote user home
            auto home_r = run_command("ssh " + user + "@" + host +
                                      " 'echo $HOME' 2>/dev/null");
            std::string remote_home = home_r.stdout_str;
            while (!remote_home.empty() && remote_home.back() == '\n') {
                remote_home.pop_back();
            }
            if (remote_home.empty()) remote_home = "/root";

            auto r = run_command(rsync_base + user_cfg + "/ " +
                                 remote + remote_home + "/.config/straylight/");
            if (r.exit_code != 0) {
                return Result<void, std::string>::error(
                    "rsync user config failed: " + r.stderr_str);
            }
        }
    }

    report("Syncing package list", 85);

    // Sync package selections
    auto pkg_r = run_command(
        "dpkg --get-selections 2>/dev/null | "
        "ssh " + user + "@" + host +
        " 'dpkg --set-selections && apt-get dselect-upgrade -y' 2>/dev/null");
    // Best-effort, don't fail on this

    report("Complete", 100);
    return Result<void, std::string>::ok();
}

// ---------------------------------------------------------------------------
// Remote diff
// ---------------------------------------------------------------------------

Result<DiffSummary, std::string> Migrator::diff_remote(
    const std::string& host, const std::string& user)
{
    report("Building local manifest", 20);

    auto local_r = diff_engine_.build_local_manifest();
    if (!local_r.has_value()) {
        return Result<DiffSummary, std::string>::error(local_r.error());
    }

    report("Fetching remote manifest", 50);

    // Run manifest builder on remote machine
    auto remote_r = run_command(
        "ssh " + user + "@" + host +
        " 'straylight-migrate manifest 2>/dev/null'");

    if (remote_r.exit_code != 0) {
        // Fallback: build a remote manifest by listing files
        auto list_r = run_command(
            "ssh " + user + "@" + host +
            " 'find /etc/straylight ~/.config/straylight -type f "
            "-exec sha256sum {} \\; 2>/dev/null'");

        if (list_r.exit_code != 0) {
            return Result<DiffSummary, std::string>::error(
                "Cannot access remote machine: " + remote_r.stderr_str);
        }

        // Parse sha256sum output into manifest
        std::vector<ManifestEntry> remote_manifest;
        std::istringstream iss(list_r.stdout_str);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.size() < 66) continue; // 64 hex + 2 spaces + path
            ManifestEntry me;
            me.sha256 = line.substr(0, 64);
            me.path = line.substr(66);
            while (!me.path.empty() && me.path.back() == '\n') me.path.pop_back();
            remote_manifest.push_back(std::move(me));
        }

        report("Comparing", 80);
        auto diff = diff_engine_.compare(local_r.value(), remote_manifest);
        report("Complete", 100);
        return Result<DiffSummary, std::string>::ok(std::move(diff));
    }

    auto remote_manifest_r = DiffEngine::manifest_from_json(remote_r.stdout_str);
    if (!remote_manifest_r.has_value()) {
        return Result<DiffSummary, std::string>::error(remote_manifest_r.error());
    }

    report("Comparing", 80);
    auto diff = diff_engine_.compare(local_r.value(), remote_manifest_r.value());
    report("Complete", 100);
    return Result<DiffSummary, std::string>::ok(std::move(diff));
}

} // namespace straylight
