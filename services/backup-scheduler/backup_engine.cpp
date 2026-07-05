// services/backup-scheduler/backup_engine.cpp
#include "backup_engine.h"

#include <straylight/log.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <sys/wait.h>

namespace straylight {

namespace fs = std::filesystem;

namespace {

std::string shell_quote(const std::string& value) {
    if (value.empty()) return "''";

    std::string quoted = "'";
    for (char c : value) {
        if (c == '\'') {
            quoted += "'\\''";
        } else {
            quoted += c;
        }
    }
    quoted += "'";
    return quoted;
}

int exec_return_code(const std::string& cmd) {
    int status = std::system(cmd.c_str());
    if (status == -1) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return status;
}

std::string time_to_string(std::chrono::system_clock::time_point tp) {
    auto t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d_%H%M%S", &tm);
    return buf;
}

std::string time_to_iso(std::chrono::system_clock::time_point tp) {
    auto t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    return buf;
}

std::chrono::system_clock::time_point parse_iso(const std::string& s) {
    std::tm tm{};
    std::istringstream ss(s);
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    return std::chrono::system_clock::from_time_t(std::mktime(&tm));
}

} // namespace

std::string BackupEngine::generate_id() const {
    auto now = std::chrono::system_clock::now();
    auto ts = time_to_string(now);
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(1000, 9999);
    return "bk-" + ts + "-" + std::to_string(dist(gen));
}

Result<void, SLError> BackupEngine::init(const fs::path& config_path) {
    std::ifstream ifs(config_path);
    if (!ifs) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::NotFound, "Config not found: " + config_path.string()});
    }

    try {
        nlohmann::json cfg;
        ifs >> cfg;

        backup_dir_ = cfg.value("backup_directory", "/var/lib/straylight/backups");
        db_path_ = fs::path(backup_dir_) / "backup.db.json";

        // Load remotes
        if (cfg.contains("remotes") && cfg["remotes"].is_array()) {
            for (const auto& rj : cfg["remotes"]) {
                RemoteTarget rt;
                rt.name = rj.value("name", "");
                rt.host = rj.value("host", "");
                rt.path = rj.value("path", "/backups");
                rt.ssh_key = rj.value("ssh_key", "");
                rt.port = rj.value("port", 22);
                rt.bandwidth_limit_kbps = rj.value("bandwidth_limit_kbps", 0);
                if (!rt.name.empty()) {
                    remotes_[rt.name] = rt;
                }
            }
        }

        // Load schedules
        if (cfg.contains("schedules") && cfg["schedules"].is_array()) {
            for (const auto& sj : cfg["schedules"]) {
                BackupSchedule bs;
                bs.name = sj.value("name", "");
                bs.source = sj.value("source", "/");
                bs.cron_expression = sj.value("schedule", "daily");
                bs.enabled = sj.value("enabled", false);
                bs.encrypt = sj.value("encrypt", false);
                bs.gpg_recipient = sj.value("gpg_recipient", "");

                if (sj.contains("remote_targets") && sj["remote_targets"].is_array()) {
                    for (const auto& rt : sj["remote_targets"]) {
                        bs.remote_targets.push_back(rt.get<std::string>());
                    }
                }

                if (sj.contains("retention")) {
                    bs.retention.keep_daily = sj["retention"].value("daily", 7);
                    bs.retention.keep_weekly = sj["retention"].value("weekly", 4);
                    bs.retention.keep_monthly = sj["retention"].value("monthly", 12);
                }

                if (!bs.name.empty()) {
                    schedules_.push_back(std::move(bs));
                }
            }
        }

        // Create backup directory
        std::error_code ec;
        fs::create_directories(backup_dir_, ec);

        // Load existing database
        load_database();

        SL_INFO("backup: initialized with {} schedules, {} remotes, {} records",
                schedules_.size(), remotes_.size(), records_.size());
        return Result<void, SLError>::ok();

    } catch (const std::exception& e) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::ParseError, std::string("Config parse error: ") + e.what()});
    }
}

Result<BackupRecord, SLError> BackupEngine::run_backup(const std::string& schedule_name) {
    std::lock_guard lock(mutex_);

    // Find the schedule
    const BackupSchedule* target = nullptr;
    for (const auto& s : schedules_) {
        if (schedule_name.empty() && !s.enabled) {
            continue;
        }
        if (schedule_name.empty() || s.name == schedule_name) {
            target = &s;
            break;
        }
    }

    if (!target && !schedule_name.empty()) {
        return Result<BackupRecord, SLError>::error(
            SLError{SLErrorCode::NotFound, "Schedule not found: " + schedule_name});
    }

    if (!target && !schedules_.empty()) {
        for (const auto& s : schedules_) {
            if (s.enabled) {
                target = &s;
                break;
            }
        }
    }

    if (!target) {
        return Result<BackupRecord, SLError>::error(
            SLError{SLErrorCode::NotFound, "No enabled schedules configured"});
    }

    // Run local snapshot
    auto snap_res = snapshot(target->source, target->encrypt, target->gpg_recipient);
    if (!snap_res.has_value()) {
        return snap_res;
    }

    auto record = snap_res.value();

    // Rsync to each remote
    for (const auto& remote_name : target->remote_targets) {
        auto it = remotes_.find(remote_name);
        if (it != remotes_.end()) {
            auto rsync_res = rsync_to_remote(record.destination, it->second);
            if (!rsync_res.has_value()) {
                SL_WARN("backup: rsync to '{}' failed: {}",
                        remote_name, rsync_res.error().message());
            }
        }
    }

    last_run_[target->name] = std::chrono::system_clock::now();
    return Result<BackupRecord, SLError>::ok(record);
}

Result<BackupRecord, SLError> BackupEngine::snapshot(
    const std::string& source, bool encrypt, const std::string& gpg_recipient) {

    auto now = std::chrono::system_clock::now();
    std::string id = generate_id();
    fs::path dest = backup_dir_ / (id + ".tar.zst");

    SL_INFO("backup: creating snapshot of '{}' -> '{}'", source, dest.string());

    // Create compressed archive using tar + zstd
    std::string cmd = "tar -cf - " + shell_quote(source) +
                      " 2>/dev/null | zstd -3 -o " +
                      shell_quote(dest.string()) + " 2>&1";
    int rc = exec_return_code(cmd);

    BackupRecord record;
    record.id = id;
    record.type = "snapshot";
    record.source = source;
    record.destination = dest.string();
    record.timestamp = now;
    record.encrypted = false;

    if (rc != 0) {
        record.status = "failed";
        record.error_message = "tar/zstd failed with exit code " + std::to_string(rc);
        records_.push_back(record);
        save_database();
        return Result<BackupRecord, SLError>::error(
            SLError{SLErrorCode::Internal, record.error_message});
    }

    // Get file size
    std::error_code ec;
    record.size_bytes = fs::file_size(dest, ec);

    // Encrypt if requested
    if (encrypt && !gpg_recipient.empty()) {
        auto enc_res = encrypt_file(dest, gpg_recipient);
        if (enc_res.has_value()) {
            fs::remove(dest, ec);
            record.destination = enc_res.value().string();
            record.encrypted = true;
            record.size_bytes = fs::file_size(enc_res.value(), ec);
        } else {
            SL_WARN("backup: encryption failed: {}", enc_res.error().message());
        }
    }

    record.status = "ok";
    records_.push_back(record);
    save_database();

    SL_INFO("backup: snapshot complete — id={}, size={}", id, record.size_bytes);
    return Result<BackupRecord, SLError>::ok(record);
}

Result<BackupRecord, SLError> BackupEngine::rsync_to_remote(
    const std::string& source, const RemoteTarget& target) {

    auto now = std::chrono::system_clock::now();
    std::string id = generate_id();

    std::string remote_path = target.host + ":" + target.path;
    std::string cmd = "rsync -az --compress";

    if (target.bandwidth_limit_kbps > 0) {
        cmd += " --bwlimit=" + std::to_string(target.bandwidth_limit_kbps);
    }
    std::string ssh_cmd = "ssh";
    if (!target.ssh_key.empty()) {
        ssh_cmd += " -i " + shell_quote(target.ssh_key);
    }
    if (!target.ssh_key.empty() || target.port != 22) {
        ssh_cmd += " -p " + std::to_string(target.port);
        cmd += " -e " + shell_quote(ssh_cmd);
    }
    cmd += " " + shell_quote(source) + " " + shell_quote(remote_path) + " 2>&1";

    SL_INFO("backup: rsync '{}' -> '{}'", source, remote_path);
    int rc = exec_return_code(cmd);

    BackupRecord record;
    record.id = id;
    record.type = "rsync";
    record.source = source;
    record.destination = remote_path;
    record.timestamp = now;

    if (rc != 0) {
        record.status = "failed";
        record.error_message = "rsync failed with exit code " + std::to_string(rc);
        records_.push_back(record);
        save_database();
        return Result<BackupRecord, SLError>::error(
            SLError{SLErrorCode::Internal, record.error_message});
    }

    record.status = "ok";
    records_.push_back(record);
    save_database();

    return Result<BackupRecord, SLError>::ok(record);
}

std::vector<BackupRecord> BackupEngine::list_backups() const {
    std::lock_guard lock(mutex_);
    return records_;
}

Result<void, SLError> BackupEngine::restore(const std::string& backup_id,
                                              const std::string& target_path) {
    std::lock_guard lock(mutex_);

    const BackupRecord* rec = nullptr;
    for (const auto& r : records_) {
        if (r.id == backup_id) {
            rec = &r;
            break;
        }
    }

    if (!rec) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::NotFound, "Backup not found: " + backup_id});
    }

    if (rec->type != "snapshot") {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::InvalidArgument,
                    "Only snapshot backups can be restored locally"});
    }

    fs::path source = rec->destination;
    if (!fs::exists(source)) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::NotFound, "Backup file missing: " + source.string()});
    }

    std::string dest = target_path.empty() ? "/" : target_path;

    // Decrypt if needed
    if (rec->encrypted) {
        std::string dec_path = "/tmp/straylight-restore-" + backup_id + ".tar.zst";
        std::string cmd = "gpg -d -o " + shell_quote(dec_path) + " " +
                          shell_quote(source.string()) + " 2>&1";
        if (exec_return_code(cmd) != 0) {
            return Result<void, SLError>::error(
                SLError{SLErrorCode::Internal, "Decryption failed"});
        }
        source = dec_path;
    }

    // Decompress and extract
    std::string cmd = "zstd -d " + shell_quote(source.string()) +
                      " --stdout | tar -xf - -C " + shell_quote(dest) + " 2>&1";
    SL_INFO("backup: restoring {} to {}", backup_id, dest);

    if (exec_return_code(cmd) != 0) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::Internal, "Restore extraction failed"});
    }

    SL_INFO("backup: restore complete for {}", backup_id);
    return Result<void, SLError>::ok();
}

Result<bool, SLError> BackupEngine::verify(const std::string& backup_id) {
    std::lock_guard lock(mutex_);

    BackupRecord* rec = nullptr;
    for (auto& r : records_) {
        if (r.id == backup_id) {
            rec = &r;
            break;
        }
    }

    if (!rec) {
        return Result<bool, SLError>::error(
            SLError{SLErrorCode::NotFound, "Backup not found: " + backup_id});
    }

    fs::path source = rec->destination;
    if (!fs::exists(source)) {
        rec->verified = false;
        save_database();
        return Result<bool, SLError>::ok(false);
    }

    // Verify by testing decompression
    std::string cmd;
    if (rec->encrypted) {
        cmd = "gpg -d " + shell_quote(source.string()) + " 2>/dev/null | zstd -t 2>&1";
    } else {
        cmd = "zstd -t " + shell_quote(source.string()) + " 2>&1";
    }

    bool valid = exec_return_code(cmd) == 0;
    rec->verified = valid;
    save_database();

    return Result<bool, SLError>::ok(valid);
}

Result<void, SLError> BackupEngine::add_remote(const RemoteTarget& target) {
    std::lock_guard lock(mutex_);
    if (target.name.empty()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::InvalidArgument, "Remote name cannot be empty"});
    }
    remotes_[target.name] = target;
    return Result<void, SLError>::ok();
}

Result<void, SLError> BackupEngine::remove_remote(const std::string& name) {
    std::lock_guard lock(mutex_);
    auto it = remotes_.find(name);
    if (it == remotes_.end()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::NotFound, "Remote not found: " + name});
    }
    remotes_.erase(it);
    return Result<void, SLError>::ok();
}

std::vector<RemoteTarget> BackupEngine::list_remotes() const {
    std::lock_guard lock(mutex_);
    std::vector<RemoteTarget> result;
    for (const auto& [_, rt] : remotes_) {
        result.push_back(rt);
    }
    return result;
}

std::vector<BackupSchedule> BackupEngine::list_schedules() const {
    std::lock_guard lock(mutex_);
    return schedules_;
}

Result<int, SLError> BackupEngine::apply_retention(const RetentionPolicy& policy) {
    std::lock_guard lock(mutex_);
    auto now = std::chrono::system_clock::now();
    int removed = 0;

    // Sort records by timestamp (newest first)
    std::vector<BackupRecord> sorted = records_;
    std::sort(sorted.begin(), sorted.end(),
              [](const BackupRecord& a, const BackupRecord& b) {
                  return a.timestamp > b.timestamp;
              });

    int daily_count = 0;
    int weekly_count = 0;
    int monthly_count = 0;
    std::vector<std::string> to_remove;

    for (const auto& rec : sorted) {
        auto age = std::chrono::duration_cast<std::chrono::hours>(now - rec.timestamp);
        int days = static_cast<int>(age.count() / 24);

        bool keep = false;
        if (days < 7) {
            if (daily_count < policy.keep_daily) { keep = true; daily_count++; }
        } else if (days < 30) {
            if (weekly_count < policy.keep_weekly) { keep = true; weekly_count++; }
        } else {
            if (monthly_count < policy.keep_monthly) { keep = true; monthly_count++; }
        }

        if (!keep) {
            to_remove.push_back(rec.id);
        }
    }

    // Remove old backups
    for (const auto& id : to_remove) {
        for (auto it = records_.begin(); it != records_.end(); ++it) {
            if (it->id == id) {
                // Delete the file
                std::error_code ec;
                fs::remove(it->destination, ec);
                records_.erase(it);
                removed++;
                break;
            }
        }
    }

    if (removed > 0) {
        save_database();
    }

    return Result<int, SLError>::ok(removed);
}

bool BackupEngine::is_due(const BackupSchedule& schedule) const {
    auto it = last_run_.find(schedule.name);
    if (it == last_run_.end()) return true; // Never run

    auto now = std::chrono::system_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::hours>(now - it->second);

    if (schedule.cron_expression == "daily") return elapsed.count() >= 24;
    if (schedule.cron_expression == "weekly") return elapsed.count() >= 168;
    if (schedule.cron_expression == "monthly") return elapsed.count() >= 720;

    // Default: daily
    return elapsed.count() >= 24;
}

Result<void, SLError> BackupEngine::tick() {
    for (const auto& schedule : schedules_) {
        if (!schedule.enabled) continue;
        if (is_due(schedule)) {
            SL_INFO("backup: schedule '{}' is due, running backup", schedule.name);
            auto res = run_backup(schedule.name);
            if (!res.has_value()) {
                SL_ERROR("backup: scheduled backup '{}' failed: {}",
                        schedule.name, res.error().message());
            }

            // Apply retention after backup
            apply_retention(schedule.retention);
        }
    }
    return Result<void, SLError>::ok();
}

Result<fs::path, SLError> BackupEngine::encrypt_file(
    const fs::path& file, const std::string& recipient) {
    fs::path encrypted = file;
    encrypted += ".gpg";
    std::string cmd = "gpg --batch --yes -e -r " + shell_quote(recipient) +
                      " -o " + shell_quote(encrypted.string()) + " " +
                      shell_quote(file.string()) + " 2>&1";
    if (exec_return_code(cmd) != 0) {
        return Result<fs::path, SLError>::error(
            SLError{SLErrorCode::Internal, "GPG encryption failed"});
    }
    return Result<fs::path, SLError>::ok(encrypted);
}

void BackupEngine::save_database() const {
    nlohmann::json j;
    j["records"] = nlohmann::json::array();
    for (const auto& r : records_) {
        nlohmann::json rj;
        rj["id"] = r.id;
        rj["type"] = r.type;
        rj["source"] = r.source;
        rj["destination"] = r.destination;
        rj["timestamp"] = time_to_iso(r.timestamp);
        rj["size_bytes"] = r.size_bytes;
        rj["encrypted"] = r.encrypted;
        rj["verified"] = r.verified;
        rj["status"] = r.status;
        rj["error_message"] = r.error_message;
        j["records"].push_back(rj);
    }

    std::error_code ec;
    fs::create_directories(db_path_.parent_path(), ec);
    std::ofstream ofs(db_path_);
    if (ofs) ofs << j.dump(2) << "\n";
}

void BackupEngine::load_database() {
    if (!fs::exists(db_path_)) return;
    std::ifstream ifs(db_path_);
    if (!ifs) return;

    try {
        nlohmann::json j;
        ifs >> j;

        if (j.contains("records") && j["records"].is_array()) {
            for (const auto& rj : j["records"]) {
                BackupRecord r;
                r.id = rj.value("id", "");
                r.type = rj.value("type", "snapshot");
                r.source = rj.value("source", "");
                r.destination = rj.value("destination", "");
                r.timestamp = parse_iso(rj.value("timestamp", "2000-01-01T00:00:00"));
                r.size_bytes = rj.value("size_bytes", uint64_t(0));
                r.encrypted = rj.value("encrypted", false);
                r.verified = rj.value("verified", false);
                r.status = rj.value("status", "unknown");
                r.error_message = rj.value("error_message", "");
                records_.push_back(std::move(r));
            }
        }
    } catch (...) {
        SL_WARN("backup: failed to load database");
    }
}

} // namespace straylight
