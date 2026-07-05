// tools/update/update_manager.cpp
// Full implementation of system update management for StrayLight OS.

#include "update_manager.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <random>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

namespace straylight {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

UpdateManager::UpdateManager() {
    ensure_dirs();
}

UpdateManager::~UpdateManager() = default;

void UpdateManager::ensure_dirs() const {
    fs::create_directories("/var/lib/straylight/updates");
}

Result<std::string, std::string> UpdateManager::run_cmd(const std::string& cmd) const {
    std::array<char, 8192> buffer{};
    std::string output;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return Result<std::string, std::string>::error(
            "popen failed: " + std::string(strerror(errno)));
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

std::string UpdateManager::generate_id() const {
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&tt, &tm);

    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y%m%d-%H%M%S");

    // Add a short random suffix
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(1000, 9999);
    ss << "-" << dist(gen);

    return ss.str();
}

// ---------------------------------------------------------------------------
// Snapshot integration
// ---------------------------------------------------------------------------

Result<std::string, std::string> UpdateManager::create_snapshot(const std::string& label) const {
    std::string name = "pre-update-" + label;
    auto res = run_cmd("straylight-snapshot save '" + name +
                       "' --description 'Pre-update snapshot' 2>/dev/null");
    if (!res.has_value()) {
        return Result<std::string, std::string>::error(
            "failed to create pre-update snapshot: " + res.error());
    }
    return Result<std::string, std::string>::ok(name);
}

Result<void, std::string> UpdateManager::restore_snapshot(const std::string& name) const {
    auto res = run_cmd("straylight-snapshot restore '" + name + "' 2>/dev/null");
    if (!res.has_value()) {
        return Result<void, std::string>::error(
            "failed to restore snapshot: " + res.error());
    }
    return Result<void, std::string>::ok();
}

Result<std::string, std::string> UpdateManager::last_snapshot() const {
    // Read history to find the most recent successful update's snapshot
    auto hist = history();
    if (!hist.has_value()) {
        return Result<std::string, std::string>::error("no update history");
    }

    const auto& records = hist.value();
    for (auto it = records.rbegin(); it != records.rend(); ++it) {
        if (it->success && !it->snapshot_name.empty()) {
            return Result<std::string, std::string>::ok(it->snapshot_name);
        }
    }

    return Result<std::string, std::string>::error("no pre-update snapshots found");
}

// ---------------------------------------------------------------------------
// check
// ---------------------------------------------------------------------------

std::vector<PackageUpdate> UpdateManager::parse_upgradable(const std::string& output) const {
    std::vector<PackageUpdate> updates;

    // Format: "package/suite current_ver upgradable_ver arch"
    // Example: "vim/stable 2:9.0.1378-2 amd64 [upgradable from: 2:9.0.1000-4]"
    std::regex pkg_re(R"(^(\S+)/(\S+)\s+(\S+)\s+\S+\s+\[upgradable from:\s+(\S+)\])");

    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        std::smatch m;
        if (std::regex_search(line, m, pkg_re)) {
            PackageUpdate update;
            update.name = m[1].str();
            update.section = m[2].str();
            update.new_version = m[3].str();
            update.current_version = m[4].str();

            // Check if security update
            update.is_security = (update.section.find("security") != std::string::npos);

            updates.push_back(update);
        }
    }

    return updates;
}

Result<std::vector<PackageUpdate>, std::string> UpdateManager::check() const {
    // Run apt update first
    auto update_res = run_cmd("apt-get update -qq 2>/dev/null");
    if (!update_res.has_value()) {
        // Try without root
        update_res = run_cmd("apt update 2>/dev/null");
    }

    // List upgradable packages
    auto list_res = run_cmd("apt list --upgradable 2>/dev/null");
    if (!list_res.has_value()) {
        return Result<std::vector<PackageUpdate>, std::string>::error(
            "failed to check for updates: " + list_res.error());
    }

    auto updates = parse_upgradable(list_res.value());

    // Get download sizes
    if (!updates.empty()) {
        auto sim_res = run_cmd("apt-get upgrade --simulate 2>/dev/null");
        if (sim_res.has_value()) {
            // Parse "Need to get X kB of archives"
            std::regex size_re(R"(Need to get (\d+\.?\d*)\s*(\w+))");
            std::smatch m;
            std::string sim = sim_res.value();
            if (std::regex_search(sim, m, size_re)) {
                double size = std::stod(m[1].str());
                std::string unit = m[2].str();
                uint64_t total_bytes = 0;
                if (unit == "kB") total_bytes = static_cast<uint64_t>(size * 1024);
                else if (unit == "MB") total_bytes = static_cast<uint64_t>(size * 1024 * 1024);
                else if (unit == "GB") total_bytes = static_cast<uint64_t>(size * 1024 * 1024 * 1024);

                // Distribute evenly (approximation)
                if (!updates.empty()) {
                    uint64_t per_pkg = total_bytes / updates.size();
                    for (auto& u : updates) u.download_size = per_pkg;
                }
            }
        }
    }

    return Result<std::vector<PackageUpdate>, std::string>::ok(updates);
}

// ---------------------------------------------------------------------------
// upgrade
// ---------------------------------------------------------------------------

Result<UpdateRecord, std::string> UpdateManager::upgrade(bool auto_snapshot,
                                                           bool security_only,
                                                           bool dry_run) {
    UpdateRecord record;
    record.id = generate_id();
    record.timestamp = std::chrono::system_clock::now();

    // Check what's available first
    auto check_res = check();
    if (!check_res.has_value()) {
        record.success = false;
        record.error_message = check_res.error();
        write_history(record);
        return Result<UpdateRecord, std::string>::error(check_res.error());
    }

    auto updates = check_res.value();
    if (updates.empty()) {
        record.success = true;
        write_history(record);
        return Result<UpdateRecord, std::string>::ok(record);
    }

    // Filter for security-only if requested
    if (security_only) {
        std::vector<PackageUpdate> sec_only;
        for (const auto& u : updates) {
            if (u.is_security) sec_only.push_back(u);
        }
        updates = sec_only;
        if (updates.empty()) {
            record.success = true;
            write_history(record);
            return Result<UpdateRecord, std::string>::ok(record);
        }
    }

    // Create pre-update snapshot
    if (auto_snapshot && !dry_run) {
        auto snap_res = create_snapshot(record.id);
        if (snap_res.has_value()) {
            record.snapshot_name = snap_res.value();
        }
        // Continue even if snapshot fails
    }

    // Build upgrade command
    std::ostringstream cmd;
    if (security_only) {
        // Install only security packages by name
        cmd << "DEBIAN_FRONTEND=noninteractive apt-get install -y";
        for (const auto& u : updates) {
            cmd << " " << u.name << "=" << u.new_version;
        }
    } else {
        cmd << "DEBIAN_FRONTEND=noninteractive apt-get upgrade -y";
    }

    if (dry_run) {
        cmd << " --simulate";
    }
    cmd << " 2>&1";

    auto upgrade_res = run_cmd(cmd.str());
    if (!upgrade_res.has_value()) {
        record.success = false;
        record.error_message = upgrade_res.error();
        write_history(record);
        return Result<UpdateRecord, std::string>::error(
            "upgrade failed: " + upgrade_res.error());
    }

    // Parse results
    std::string output = upgrade_res.value();
    for (const auto& u : updates) {
        record.packages_upgraded.push_back(u.name + " " + u.current_version +
                                            " -> " + u.new_version);
    }

    // Check for newly installed packages
    std::regex inst_re(R"(Setting up (\S+)\s)");
    auto it = std::sregex_iterator(output.begin(), output.end(), inst_re);
    for (; it != std::sregex_iterator(); ++it) {
        std::string pkg = (*it)[1].str();
        bool was_upgrade = false;
        for (const auto& u : updates) {
            if (u.name == pkg) { was_upgrade = true; break; }
        }
        if (!was_upgrade) {
            record.packages_installed.push_back(pkg);
        }
    }

    record.success = true;

    if (!dry_run) {
        write_history(record);
    }

    return Result<UpdateRecord, std::string>::ok(record);
}

// ---------------------------------------------------------------------------
// rollback
// ---------------------------------------------------------------------------

Result<void, std::string> UpdateManager::rollback() const {
    auto snap_res = last_snapshot();
    if (!snap_res.has_value()) {
        return Result<void, std::string>::error(snap_res.error());
    }

    return restore_snapshot(snap_res.value());
}

// ---------------------------------------------------------------------------
// history
// ---------------------------------------------------------------------------

Result<void, std::string> UpdateManager::write_history(const UpdateRecord& record) const {
    ensure_dirs();

    // Read existing history
    std::string content;
    {
        std::ifstream f(kHistoryFile);
        if (f.is_open()) {
            content = std::string((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
        }
    }

    if (content.empty()) {
        content = "{ \"records\": [] }";
    }

    // Format timestamp
    auto tt = std::chrono::system_clock::to_time_t(record.timestamp);
    std::tm tm{};
    localtime_r(&tt, &tm);
    std::ostringstream ts;
    ts << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");

    // Build JSON entry
    std::ostringstream json;
    json << "    {\n"
         << "      \"id\": \"" << record.id << "\",\n"
         << "      \"timestamp\": \"" << ts.str() << "\",\n"
         << "      \"snapshot\": \"" << record.snapshot_name << "\",\n"
         << "      \"success\": " << (record.success ? "true" : "false") << ",\n";

    if (!record.error_message.empty()) {
        json << "      \"error\": \"" << record.error_message << "\",\n";
    }

    json << "      \"upgraded\": [";
    for (size_t i = 0; i < record.packages_upgraded.size(); ++i) {
        json << "\"" << record.packages_upgraded[i] << "\"";
        if (i + 1 < record.packages_upgraded.size()) json << ", ";
    }
    json << "],\n"
         << "      \"installed\": [";
    for (size_t i = 0; i < record.packages_installed.size(); ++i) {
        json << "\"" << record.packages_installed[i] << "\"";
        if (i + 1 < record.packages_installed.size()) json << ", ";
    }
    json << "],\n"
         << "      \"removed\": [";
    for (size_t i = 0; i < record.packages_removed.size(); ++i) {
        json << "\"" << record.packages_removed[i] << "\"";
        if (i + 1 < record.packages_removed.size()) json << ", ";
    }
    json << "]\n"
         << "    }";

    // Insert before closing bracket
    auto arr_end = content.rfind(']');
    if (arr_end == std::string::npos) {
        return Result<void, std::string>::error("malformed history file");
    }

    bool has_entries = (content.find('{', content.find('[')) != std::string::npos &&
                        content.find('{', content.find('[')) < arr_end);
    std::string insert = (has_entries ? ",\n" : "\n") + json.str() + "\n";
    content.insert(arr_end, insert);

    std::ofstream out(kHistoryFile);
    if (!out.is_open()) {
        return Result<void, std::string>::error("cannot write history file");
    }
    out << content;
    return Result<void, std::string>::ok();
}

Result<std::vector<UpdateRecord>, std::string> UpdateManager::history() const {
    std::vector<UpdateRecord> records;
    std::ifstream f(kHistoryFile);
    if (!f.is_open()) {
        return Result<std::vector<UpdateRecord>, std::string>::ok(records);
    }

    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());

    auto arr_start = content.find('[');
    auto arr_end = content.rfind(']');
    if (arr_start == std::string::npos || arr_end == std::string::npos) {
        return Result<std::vector<UpdateRecord>, std::string>::ok(records);
    }

    std::string arr = content.substr(arr_start + 1, arr_end - arr_start - 1);
    size_t pos = 0;

    while (true) {
        auto obj_start = arr.find('{', pos);
        if (obj_start == std::string::npos) break;

        int depth = 1;
        auto obj_end = obj_start + 1;
        while (obj_end < arr.size() && depth > 0) {
            if (arr[obj_end] == '{') ++depth;
            else if (arr[obj_end] == '}') --depth;
            ++obj_end;
        }

        std::string entry = arr.substr(obj_start, obj_end - obj_start);

        UpdateRecord record;
        std::regex id_re(R"("id"\s*:\s*"([^"]*)")");
        std::regex ts_re(R"("timestamp"\s*:\s*"([^"]*)")");
        std::regex snap_re(R"("snapshot"\s*:\s*"([^"]*)")");
        std::regex suc_re(R"("success"\s*:\s*(true|false))");
        std::regex err_re(R"("error"\s*:\s*"([^"]*)")");

        std::smatch m;
        if (std::regex_search(entry, m, id_re)) record.id = m[1].str();
        if (std::regex_search(entry, m, snap_re)) record.snapshot_name = m[1].str();
        if (std::regex_search(entry, m, suc_re)) record.success = (m[1].str() == "true");
        if (std::regex_search(entry, m, err_re)) record.error_message = m[1].str();

        if (std::regex_search(entry, m, ts_re)) {
            std::string ts_str = m[1].str();
            std::tm tm{};
            std::istringstream ss(ts_str);
            ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
            record.timestamp = std::chrono::system_clock::from_time_t(mktime(&tm));
        }

        // Parse package arrays
        auto parse_array = [&](const std::string& key) -> std::vector<std::string> {
            std::vector<std::string> items;
            std::string search = "\"" + key + "\"";
            auto key_pos = entry.find(search);
            if (key_pos == std::string::npos) return items;
            auto a_start = entry.find('[', key_pos);
            auto a_end = entry.find(']', a_start);
            if (a_start == std::string::npos || a_end == std::string::npos) return items;
            std::string arr_str = entry.substr(a_start + 1, a_end - a_start - 1);
            std::regex item_re(R"("([^"]*)")");
            auto it = std::sregex_iterator(arr_str.begin(), arr_str.end(), item_re);
            for (; it != std::sregex_iterator(); ++it) {
                items.push_back((*it)[1].str());
            }
            return items;
        };

        record.packages_upgraded = parse_array("upgraded");
        record.packages_installed = parse_array("installed");
        record.packages_removed = parse_array("removed");

        records.push_back(record);
        pos = obj_end;
    }

    return Result<std::vector<UpdateRecord>, std::string>::ok(records);
}

// ---------------------------------------------------------------------------
// schedule
// ---------------------------------------------------------------------------

Result<void, std::string> UpdateManager::schedule(const std::string& cron_expr) {
    // Write schedule config
    std::ofstream out(kScheduleFile);
    if (!out.is_open()) {
        return Result<void, std::string>::error("cannot write schedule file");
    }
    out << "{\n"
        << "  \"enabled\": true,\n"
        << "  \"cron\": \"" << cron_expr << "\",\n"
        << "  \"command\": \"straylight-update upgrade --auto-snapshot\"\n"
        << "}\n";
    out.close();

    // Register with straylight-cron
    auto res = run_cmd("straylight-cron-cli add 'auto-update' --cron='" +
                       cron_expr + "' --exec='straylight-update upgrade --auto-snapshot' 2>/dev/null");
    if (!res.has_value()) {
        // Fall back to systemd timer
        std::string timer_path = "/etc/systemd/system/straylight-update.timer";
        std::ofstream timer(timer_path);
        if (timer.is_open()) {
            timer << "[Unit]\n"
                  << "Description=StrayLight OS automatic updates\n"
                  << "\n"
                  << "[Timer]\n"
                  << "OnCalendar=" << cron_expr << "\n"
                  << "Persistent=true\n"
                  << "\n"
                  << "[Install]\n"
                  << "WantedBy=timers.target\n";
            timer.close();

            std::string service_path = "/etc/systemd/system/straylight-update.service";
            std::ofstream service(service_path);
            if (service.is_open()) {
                service << "[Unit]\n"
                        << "Description=StrayLight OS system update\n"
                        << "\n"
                        << "[Service]\n"
                        << "Type=oneshot\n"
                        << "ExecStart=/usr/bin/straylight-update upgrade --auto-snapshot\n";
                service.close();
            }

            run_cmd("systemctl daemon-reload 2>/dev/null");
            run_cmd("systemctl enable --now straylight-update.timer 2>/dev/null");
        }
    }

    return Result<void, std::string>::ok();
}

Result<void, std::string> UpdateManager::unschedule() {
    // Remove schedule config
    if (fs::exists(kScheduleFile)) {
        fs::remove(kScheduleFile);
    }

    // Remove from straylight-cron
    run_cmd("straylight-cron-cli remove 'auto-update' 2>/dev/null");

    // Remove systemd timer
    run_cmd("systemctl disable --now straylight-update.timer 2>/dev/null");
    if (fs::exists("/etc/systemd/system/straylight-update.timer")) {
        fs::remove("/etc/systemd/system/straylight-update.timer");
    }
    if (fs::exists("/etc/systemd/system/straylight-update.service")) {
        fs::remove("/etc/systemd/system/straylight-update.service");
    }
    run_cmd("systemctl daemon-reload 2>/dev/null");

    return Result<void, std::string>::ok();
}

Result<std::string, std::string> UpdateManager::get_schedule() const {
    std::ifstream f(kScheduleFile);
    if (!f.is_open()) {
        return Result<std::string, std::string>::error("no scheduled updates");
    }

    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());

    std::regex cron_re(R"("cron"\s*:\s*"([^"]*)")");
    std::smatch m;
    if (std::regex_search(content, m, cron_re)) {
        return Result<std::string, std::string>::ok(m[1].str());
    }

    return Result<std::string, std::string>::error("no cron expression found in schedule");
}

// ---------------------------------------------------------------------------
// hold / unhold
// ---------------------------------------------------------------------------

Result<void, std::string> UpdateManager::hold(const std::string& package) {
    auto res = run_cmd("apt-mark hold " + package + " 2>/dev/null");
    if (!res.has_value()) {
        return Result<void, std::string>::error("failed to hold package: " + res.error());
    }
    return Result<void, std::string>::ok();
}

Result<void, std::string> UpdateManager::unhold(const std::string& package) {
    auto res = run_cmd("apt-mark unhold " + package + " 2>/dev/null");
    if (!res.has_value()) {
        return Result<void, std::string>::error("failed to unhold package: " + res.error());
    }
    return Result<void, std::string>::ok();
}

Result<std::vector<PackageHold>, std::string> UpdateManager::list_holds() const {
    std::vector<PackageHold> holds;
    auto res = run_cmd("apt-mark showhold 2>/dev/null");
    if (!res.has_value()) {
        return Result<std::vector<PackageHold>, std::string>::ok(holds);
    }

    std::istringstream stream(res.value());
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        // Trim
        auto pos = line.find_first_not_of(" \t");
        if (pos != std::string::npos) line = line.substr(pos);
        auto end = line.find_last_not_of(" \t\n\r");
        if (end != std::string::npos) line = line.substr(0, end + 1);

        PackageHold hold;
        hold.name = line;

        // Get version
        auto ver_res = run_cmd("dpkg -s " + line + " 2>/dev/null | grep '^Version:'");
        if (ver_res.has_value()) {
            std::string ver = ver_res.value();
            auto colon = ver.find(':');
            if (colon != std::string::npos) {
                hold.version = ver.substr(colon + 2);
                // Trim newline
                auto nl = hold.version.find('\n');
                if (nl != std::string::npos) hold.version = hold.version.substr(0, nl);
            }
        }

        holds.push_back(hold);
    }

    return Result<std::vector<PackageHold>, std::string>::ok(holds);
}

// ---------------------------------------------------------------------------
// changelog
// ---------------------------------------------------------------------------

Result<std::string, std::string> UpdateManager::changelog(const std::string& package) const {
    auto res = run_cmd("apt-get changelog " + package + " 2>/dev/null");
    if (!res.has_value()) {
        // Try dpkg changelog
        res = run_cmd("dpkg -s " + package + " 2>/dev/null");
        if (!res.has_value()) {
            return Result<std::string, std::string>::error(
                "failed to get changelog for " + package);
        }
    }
    return Result<std::string, std::string>::ok(res.value());
}

// ---------------------------------------------------------------------------
// clean
// ---------------------------------------------------------------------------

Result<void, std::string> UpdateManager::clean() const {
    auto res = run_cmd("apt-get clean 2>/dev/null");
    if (!res.has_value()) {
        return Result<void, std::string>::error("failed to clean: " + res.error());
    }
    // Also remove old straylight update logs
    run_cmd("find /var/log/straylight/ -name 'update-*.log' -mtime +30 -delete 2>/dev/null");
    return Result<void, std::string>::ok();
}

Result<uint64_t, std::string> UpdateManager::clean_estimate() const {
    uint64_t total = 0;
    std::string cache_dir = "/var/cache/apt/archives";
    if (fs::exists(cache_dir)) {
        for (const auto& entry : fs::recursive_directory_iterator(cache_dir)) {
            if (entry.is_regular_file()) {
                total += entry.file_size();
            }
        }
    }
    return Result<uint64_t, std::string>::ok(total);
}

} // namespace straylight
