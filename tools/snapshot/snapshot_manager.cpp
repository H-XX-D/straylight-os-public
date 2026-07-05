// tools/snapshot/snapshot_manager.cpp
// Full implementation of btrfs / rsync snapshot management.

#include "snapshot_manager.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <sys/statvfs.h>

namespace fs = std::filesystem;

namespace straylight {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

SnapshotManager::SnapshotManager() {
    // Ensure the snapshot directory exists.
    std::error_code ec;
    fs::create_directories(kSnapshotDir, ec);
}

SnapshotManager::~SnapshotManager() = default;

Result<void, std::string> SnapshotManager::ensure_snapshot_dir() const {
    std::error_code ec;
    fs::create_directories(kSnapshotDir, ec);
    if (ec) {
        return Result<void, std::string>::error(
            "cannot create snapshot directory: " + ec.message());
    }
    return Result<void, std::string>::ok();
}

bool SnapshotManager::is_btrfs() const {
    struct statvfs buf{};
    if (statvfs("/", &buf) != 0) {
        return false;
    }
    // btrfs magic: 0x9123683E
    // statvfs doesn't expose f_type; fall back to checking /proc/mounts.
    std::ifstream mounts("/proc/mounts");
    if (!mounts.is_open()) {
        return false;
    }
    std::string line;
    while (std::getline(mounts, line)) {
        // Lines look like: /dev/sda2 / btrfs rw,... 0 0
        std::istringstream ls(line);
        std::string dev, mp, fstype;
        ls >> dev >> mp >> fstype;
        if (mp == "/" && fstype == "btrfs") {
            return true;
        }
    }
    return false;
}

std::string SnapshotManager::meta_path(const std::string& name) const {
    return std::string(kSnapshotDir) + "/" + name + ".meta.json";
}

Result<std::string, std::string> SnapshotManager::run_cmd(const std::string& cmd) const {
    std::array<char, 4096> buffer{};
    std::string output;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return Result<std::string, std::string>::error("popen failed: " +
                                                        std::string(strerror(errno)));
    }
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }
    int rc = pclose(pipe);
    if (rc != 0) {
        return Result<std::string, std::string>::error(
            "command failed (rc=" + std::to_string(rc) + "): " + cmd +
            "\noutput: " + output);
    }
    return Result<std::string, std::string>::ok(output);
}

std::string SnapshotManager::capture_service_state() const {
    auto res = run_cmd("systemctl list-units --type=service --all --output=json 2>/dev/null");
    if (res.has_value()) {
        return res.value();
    }
    return "[]";
}

size_t SnapshotManager::dir_size(const std::string& path) const {
    size_t total = 0;
    std::error_code ec;
    for (auto& entry : fs::recursive_directory_iterator(path, fs::directory_options::skip_permission_denied, ec)) {
        if (entry.is_regular_file(ec)) {
            total += static_cast<size_t>(entry.file_size(ec));
        }
    }
    return total;
}

void SnapshotManager::write_meta(const Snapshot& snap) const {
    std::string path = meta_path(snap.name);
    std::ofstream out(path);
    if (!out.is_open()) {
        return;
    }
    auto epoch = std::chrono::duration_cast<std::chrono::seconds>(
                     snap.created.time_since_epoch())
                     .count();
    // Write minimal JSON by hand — no dependency on nlohmann in tools.
    out << "{\n";
    out << "  \"name\": \"" << snap.name << "\",\n";
    out << "  \"description\": \"" << snap.description << "\",\n";
    out << "  \"created_epoch\": " << epoch << ",\n";
    out << "  \"btrfs_path\": \"" << snap.btrfs_path << "\",\n";
    out << "  \"size_bytes\": " << snap.size_bytes << ",\n";
    out << "  \"is_auto\": " << (snap.is_auto ? "true" : "false") << ",\n";
    out << "  \"service_state\": \"<captured>\"\n";
    out << "}\n";
    out.close();

    // Write the full service-state blob next to the meta.
    std::string svc_path = std::string(kSnapshotDir) + "/" + snap.name + ".services.json";
    std::ofstream svc_out(svc_path);
    if (svc_out.is_open()) {
        svc_out << snap.service_state;
    }
}

Result<SnapshotManager::Snapshot, std::string>
SnapshotManager::load_meta(const std::string& name) const {
    std::string path = meta_path(name);
    std::ifstream in(path);
    if (!in.is_open()) {
        return Result<Snapshot, std::string>::error(
            "metadata not found for snapshot '" + name + "'");
    }

    Snapshot snap;
    snap.name = name;

    // Simple line-by-line JSON field extraction (no external JSON lib).
    std::string line;
    while (std::getline(in, line)) {
        auto extract_str = [&](const std::string& key) -> std::string {
            auto pos = line.find("\"" + key + "\"");
            if (pos == std::string::npos) return {};
            auto colon = line.find(':', pos);
            if (colon == std::string::npos) return {};
            auto q1 = line.find('"', colon + 1);
            auto q2 = line.find('"', q1 + 1);
            if (q1 == std::string::npos || q2 == std::string::npos) return {};
            return line.substr(q1 + 1, q2 - q1 - 1);
        };
        auto extract_long = [&](const std::string& key) -> long long {
            auto pos = line.find("\"" + key + "\"");
            if (pos == std::string::npos) return -1;
            auto colon = line.find(':', pos);
            if (colon == std::string::npos) return -1;
            return std::atoll(line.c_str() + colon + 1);
        };

        if (!extract_str("description").empty()) {
            snap.description = extract_str("description");
        }
        if (!extract_str("btrfs_path").empty()) {
            snap.btrfs_path = extract_str("btrfs_path");
        }
        auto ep = extract_long("created_epoch");
        if (ep > 0) {
            snap.created = std::chrono::system_clock::from_time_t(
                static_cast<time_t>(ep));
        }
        auto sz = extract_long("size_bytes");
        if (sz >= 0) {
            snap.size_bytes = static_cast<size_t>(sz);
        }
        if (line.find("\"is_auto\"") != std::string::npos) {
            snap.is_auto = line.find("true") != std::string::npos;
        }
    }

    // Load service state blob.
    std::string svc_path = std::string(kSnapshotDir) + "/" + name + ".services.json";
    std::ifstream svc_in(svc_path);
    if (svc_in.is_open()) {
        std::ostringstream ss;
        ss << svc_in.rdbuf();
        snap.service_state = ss.str();
    }

    return Result<Snapshot, std::string>::ok(std::move(snap));
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

Result<SnapshotManager::Snapshot, std::string>
SnapshotManager::save(const std::string& name, const std::string& desc) {
    if (name.empty()) {
        return Result<Snapshot, std::string>::error("snapshot name must not be empty");
    }
    // Reject names with slashes or dots to avoid path-traversal.
    if (name.find('/') != std::string::npos || name.find("..") != std::string::npos) {
        return Result<Snapshot, std::string>::error("invalid snapshot name");
    }

    auto dir = ensure_snapshot_dir();
    if (!dir.has_value()) {
        return Result<Snapshot, std::string>::error(dir.error());
    }

    std::string snap_path = std::string(kSnapshotDir) + "/" + name;

    // Check for duplicate.
    if (fs::exists(snap_path)) {
        return Result<Snapshot, std::string>::error(
            "snapshot '" + name + "' already exists — delete it first");
    }

    Snapshot snap;
    snap.name = name;
    snap.description = desc;
    snap.created = std::chrono::system_clock::now();
    snap.btrfs_path = snap_path;
    snap.is_auto = false;

    // Capture systemd service states.
    snap.service_state = capture_service_state();

    if (is_btrfs()) {
        // Create a read-only btrfs snapshot of /.
        std::string cmd = "btrfs subvolume snapshot -r / " + snap_path + " 2>&1";
        auto res = run_cmd(cmd);
        if (!res.has_value()) {
            return Result<Snapshot, std::string>::error(
                "btrfs snapshot failed: " + res.error());
        }
    } else {
        // Fallback: rsync-based snapshot.
        fs::create_directories(snap_path);
        std::string cmd =
            "rsync -aAX --info=progress2 "
            "--exclude='/var/lib/straylight/snapshots' "
            "--exclude='/proc' "
            "--exclude='/sys' "
            "--exclude='/dev' "
            "--exclude='/run' "
            "--exclude='/tmp' "
            "--exclude='/var/lib/straylight/sandboxes' "
            "/ " + snap_path + "/ 2>&1";
        auto res = run_cmd(cmd);
        if (!res.has_value()) {
            return Result<Snapshot, std::string>::error(
                "rsync snapshot failed: " + res.error());
        }
    }

    snap.size_bytes = dir_size(snap_path);
    write_meta(snap);

    return Result<Snapshot, std::string>::ok(std::move(snap));
}

Result<void, std::string>
SnapshotManager::restore(const std::string& name) {
    std::string snap_path = std::string(kSnapshotDir) + "/" + name;
    if (!fs::exists(snap_path)) {
        return Result<void, std::string>::error(
            "snapshot '" + name + "' not found");
    }

    // Save current state as a pre-restore backup so rollback works.
    auto now = std::chrono::system_clock::now();
    auto epoch = std::chrono::duration_cast<std::chrono::seconds>(
                     now.time_since_epoch())
                     .count();
    std::string backup_name = "_pre_restore_" + std::to_string(epoch);

    auto backup = save(backup_name, "auto-backup before restoring '" + name + "'");
    if (!backup.has_value()) {
        return Result<void, std::string>::error(
            "failed to create pre-restore backup: " + backup.error());
    }

    // Record which snapshot is the latest pre-restore so rollback can find it.
    {
        std::ofstream marker(std::string(kSnapshotDir) + "/_last_pre_restore");
        marker << backup_name;
    }

    if (is_btrfs()) {
        // Set the snapshot as the default subvolume and prompt reboot.
        std::string id_cmd = "btrfs subvolume show " + snap_path +
                             " 2>&1 | grep 'Subvolume ID' | awk '{print $NF}'";
        auto id_res = run_cmd(id_cmd);
        if (!id_res.has_value()) {
            return Result<void, std::string>::error(
                "could not determine subvolume ID: " + id_res.error());
        }
        std::string subvol_id = id_res.value();
        // Trim whitespace.
        while (!subvol_id.empty() &&
               (subvol_id.back() == '\n' || subvol_id.back() == ' ')) {
            subvol_id.pop_back();
        }
        std::string set_cmd =
            "btrfs subvolume set-default " + subvol_id + " / 2>&1";
        auto set_res = run_cmd(set_cmd);
        if (!set_res.has_value()) {
            return Result<void, std::string>::error(
                "btrfs set-default failed: " + set_res.error());
        }
        // Write a flag file so firstboot can finalize restore.
        {
            std::ofstream flag(std::string(kSnapshotDir) + "/_pending_restore");
            flag << name;
        }
    } else {
        // rsync restore — destructive sync from snapshot to /.
        std::string cmd =
            "rsync -aAX --delete "
            "--exclude='/var/lib/straylight/snapshots' "
            "--exclude='/proc' "
            "--exclude='/sys' "
            "--exclude='/dev' "
            "--exclude='/run' "
            "--exclude='/tmp' "
            "--exclude='/var/lib/straylight/sandboxes' "
            + snap_path + "/ / 2>&1";
        auto res = run_cmd(cmd);
        if (!res.has_value()) {
            return Result<void, std::string>::error(
                "rsync restore failed: " + res.error());
        }
    }

    return Result<void, std::string>::ok();
}

Result<void, std::string> SnapshotManager::rollback() {
    std::string marker_path = std::string(kSnapshotDir) + "/_last_pre_restore";
    std::ifstream marker(marker_path);
    if (!marker.is_open()) {
        return Result<void, std::string>::error(
            "no previous restore found — nothing to roll back");
    }
    std::string backup_name;
    std::getline(marker, backup_name);
    marker.close();

    if (backup_name.empty()) {
        return Result<void, std::string>::error(
            "pre-restore marker is empty — cannot roll back");
    }

    auto res = restore(backup_name);
    if (!res.has_value()) {
        return Result<void, std::string>::error(
            "rollback restore failed: " + res.error());
    }

    // Remove the marker so double-rollback doesn't loop.
    fs::remove(marker_path);

    return Result<void, std::string>::ok();
}

std::vector<SnapshotManager::Snapshot> SnapshotManager::list() const {
    std::vector<Snapshot> result;
    std::error_code ec;
    for (auto& entry : fs::directory_iterator(kSnapshotDir, ec)) {
        if (ec) {
            return result;
        }
        if (!entry.is_directory(ec)) continue;
        std::string name = entry.path().filename().string();
        // Skip work dirs or hidden entries.
        if (name.empty() || name[0] == '.') continue;
        auto meta = load_meta(name);
        if (meta.has_value()) {
            result.push_back(std::move(meta).value());
        } else {
            // Directory exists but no metadata — create a minimal entry.
            Snapshot s;
            s.name = name;
            s.btrfs_path = entry.path().string();
            s.size_bytes = dir_size(entry.path().string());
            result.push_back(std::move(s));
        }
    }
    // Newest first.
    std::sort(result.begin(), result.end(), [](const Snapshot& a, const Snapshot& b) {
        return a.created > b.created;
    });
    return result;
}

Result<std::string, std::string>
SnapshotManager::diff(const std::string& name) {
    std::string snap_path = std::string(kSnapshotDir) + "/" + name;
    if (!fs::exists(snap_path)) {
        return Result<std::string, std::string>::error(
            "snapshot '" + name + "' not found");
    }

    if (is_btrfs()) {
        // Use btrfs send --no-data to compute a delta.
        std::string cmd =
            "btrfs send --no-data -p / " + snap_path +
            " 2>/dev/null | btrfs receive --dump 2>&1";
        auto res = run_cmd(cmd);
        if (res.has_value()) {
            return res;
        }
        // If the btrfs approach fails (e.g., / is not read-only), fall through.
    }

    // Fallback: use diff -rq.
    std::string cmd =
        "diff -rq "
        "--exclude='snapshots' "
        "--exclude='proc' "
        "--exclude='sys' "
        "--exclude='dev' "
        "--exclude='run' "
        "--exclude='tmp' "
        "/ " + snap_path + "/ 2>&1 || true";
    auto res = run_cmd(cmd);
    if (!res.has_value()) {
        return Result<std::string, std::string>::error(
            "diff failed: " + res.error());
    }
    return res;
}

Result<void, std::string>
SnapshotManager::remove(const std::string& name) {
    std::string snap_path = std::string(kSnapshotDir) + "/" + name;
    if (!fs::exists(snap_path)) {
        return Result<void, std::string>::error(
            "snapshot '" + name + "' not found");
    }

    if (is_btrfs()) {
        std::string cmd = "btrfs subvolume delete " + snap_path + " 2>&1";
        auto res = run_cmd(cmd);
        if (!res.has_value()) {
            // Fall through to rm -rf if btrfs delete fails (non-subvolume).
        } else {
            // Clean up metadata files.
            fs::remove(meta_path(name));
            fs::remove(std::string(kSnapshotDir) + "/" + name + ".services.json");
            return Result<void, std::string>::ok();
        }
    }

    // Plain directory removal.
    std::error_code ec;
    fs::remove_all(snap_path, ec);
    if (ec) {
        return Result<void, std::string>::error(
            "failed to remove snapshot directory: " + ec.message());
    }
    fs::remove(meta_path(name));
    fs::remove(std::string(kSnapshotDir) + "/" + name + ".services.json");

    return Result<void, std::string>::ok();
}

Result<void, std::string>
SnapshotManager::auto_enable(int interval_secs, int keep_count) {
    if (interval_secs <= 0) {
        return Result<void, std::string>::error("interval must be positive");
    }
    if (keep_count <= 0) {
        return Result<void, std::string>::error("keep count must be positive");
    }

    // Write a helper script that straylight-snapshot will call.
    std::string script_path = "/usr/local/libexec/straylight-auto-snapshot.sh";
    {
        fs::create_directories("/usr/local/libexec");
        std::ofstream script(script_path);
        if (!script.is_open()) {
            return Result<void, std::string>::error(
                "cannot write auto-snapshot script to " + script_path);
        }
        script << "#!/bin/bash\n"
               << "set -euo pipefail\n"
               << "NAME=\"auto-$(date +%Y%m%d-%H%M%S)\"\n"
               << "straylight-snapshot save \"$NAME\" --description \"automatic snapshot\"\n"
               << "\n"
               << "# Rotate: keep only the newest " << keep_count << " auto snapshots.\n"
               << "SNAPS=$(straylight-snapshot list 2>/dev/null | grep '\\[auto\\]' | "
               << "awk '{print $1}' | tail -n +" << (keep_count + 1) << ")\n"
               << "for S in $SNAPS; do\n"
               << "  straylight-snapshot delete \"$S\"\n"
               << "done\n";
        script.close();
        fs::permissions(script_path,
                        fs::perms::owner_exec | fs::perms::owner_read |
                            fs::perms::group_read | fs::perms::group_exec |
                            fs::perms::others_read,
                        fs::perm_options::add);
    }

    // Write systemd service unit.
    std::string svc_unit = "/etc/systemd/system/straylight-auto-snapshot.service";
    {
        std::ofstream svc(svc_unit);
        if (!svc.is_open()) {
            return Result<void, std::string>::error(
                "cannot write systemd service to " + svc_unit);
        }
        svc << "[Unit]\n"
            << "Description=StrayLight automatic snapshot\n"
            << "\n"
            << "[Service]\n"
            << "Type=oneshot\n"
            << "ExecStart=" << script_path << "\n";
    }

    // Compute OnUnitActiveSec for the timer.
    int hours = interval_secs / 3600;
    int minutes = (interval_secs % 3600) / 60;
    int seconds = interval_secs % 60;
    std::ostringstream interval_str;
    if (hours > 0) interval_str << hours << "h";
    if (minutes > 0) interval_str << minutes << "min";
    if (seconds > 0 || interval_str.str().empty()) interval_str << seconds << "s";

    // Write systemd timer unit.
    std::string timer_unit = "/etc/systemd/system/straylight-auto-snapshot.timer";
    {
        std::ofstream timer(timer_unit);
        if (!timer.is_open()) {
            return Result<void, std::string>::error(
                "cannot write systemd timer to " + timer_unit);
        }
        timer << "[Unit]\n"
              << "Description=StrayLight automatic snapshot timer\n"
              << "\n"
              << "[Timer]\n"
              << "OnBootSec=5min\n"
              << "OnUnitActiveSec=" << interval_str.str() << "\n"
              << "Persistent=true\n"
              << "\n"
              << "[Install]\n"
              << "WantedBy=timers.target\n";
    }

    // Reload systemd and enable the timer.
    auto reload = run_cmd("systemctl daemon-reload 2>&1");
    if (!reload.has_value()) {
        return Result<void, std::string>::error(
            "systemctl daemon-reload failed: " + reload.error());
    }

    auto enable = run_cmd("systemctl enable --now straylight-auto-snapshot.timer 2>&1");
    if (!enable.has_value()) {
        return Result<void, std::string>::error(
            "failed to enable timer: " + enable.error());
    }

    return Result<void, std::string>::ok();
}

} // namespace straylight
