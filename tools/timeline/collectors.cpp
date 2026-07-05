// tools/timeline/collectors.cpp
// Full implementation of system data collectors for the timeline.

#include "collectors.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;

namespace straylight {

static bool is_app_collector_noise(const std::string& cmd) {
    static const std::vector<std::string> exact = {
        "agetty", "avahi-daemon", "awk", "bash", "cat", "cc1plus",
        "cmake", "c++", "dbus-daemon", "gnome-keyring-d", "grep", "head",
        "install", "journalctl", "ld", "ninja", "nm-dispatcher",
        "pipewire", "pipewire-pulse", "ps", "sed", "sh", "sleep",
        "sort", "sshd-session", "sudo", "systemctl", "tail", "udisksd",
        "uniq", "upowerd", "watchdogd", "wireplumber", "wpa_supplicant",
        "xdg-desktop-por", "xdg-document-po", "xdg-permission-", "zsh"
    };
    if (cmd.empty() ||
        std::find(exact.begin(), exact.end(), cmd) != exact.end()) {
        return true;
    }
    if (cmd.front() == '[' || cmd.front() == '(' ||
        cmd.find(' ') != std::string::npos) {
        return true;
    }
    static const std::vector<std::string> prefixes = {
        "irq/", "kworker", "nv_", "rcu_", "straylight-", "systemd-", "usb-"
    };
    for (const auto& prefix : prefixes) {
        if (cmd.rfind(prefix, 0) == 0) return true;
    }
    return false;
}

std::string Collectors::home_dir() {
    const char* h = std::getenv("HOME");
    return h ? std::string(h) : "/root";
}

std::vector<std::string> Collectors::run_lines(const std::string& cmd) {
    std::vector<std::string> lines;
    std::array<char, 4096> buffer{};
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return lines;

    std::string output;
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
        output += buffer.data();
    }
    pclose(pipe);

    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    return lines;
}

std::chrono::system_clock::time_point
Collectors::parse_time(const std::string& s) {
    // Try epoch first.
    if (!s.empty() && (isdigit(s[0]) || s[0] == '-')) {
        long long epoch = std::atoll(s.c_str());
        if (epoch > 1000000000LL) { // Looks like a valid epoch.
            return std::chrono::system_clock::from_time_t(
                static_cast<time_t>(epoch));
        }
    }

    // Try ISO 8601: "2025-01-15 14:30:00" or "2025-01-15T14:30:00".
    std::tm tm{};
    std::string normalized = s;
    std::replace(normalized.begin(), normalized.end(), 'T', ' ');
    std::istringstream ss(normalized);
    ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
    if (!ss.fail()) {
        return std::chrono::system_clock::from_time_t(mktime(&tm));
    }

    // Try "Mon DD HH:MM:SS" format (syslog-style).
    std::memset(&tm, 0, sizeof(tm));
    ss.clear();
    ss.str(s);
    ss >> std::get_time(&tm, "%b %d %H:%M:%S");
    if (!ss.fail()) {
        // Use current year.
        auto now_t = std::chrono::system_clock::to_time_t(
            std::chrono::system_clock::now());
        std::tm now_tm{};
        localtime_r(&now_t, &now_tm);
        tm.tm_year = now_tm.tm_year;
        return std::chrono::system_clock::from_time_t(mktime(&tm));
    }

    // Fallback to now.
    return std::chrono::system_clock::now();
}

// ---------------------------------------------------------------------------
// Login/logout collector (wtmp via 'last')
// ---------------------------------------------------------------------------

std::vector<TimelineEvent> Collectors::collect_logins() {
    std::vector<TimelineEvent> events;

    auto lines = run_lines("last -F -n 200 2>/dev/null");
    for (const auto& line : lines) {
        if (line.empty() || line[0] == '\n') continue;
        if (line.find("wtmp begins") != std::string::npos) continue;
        if (line.find("reboot") != std::string::npos) {
            // System reboot entry.
            TimelineEvent e;
            e.category = "login";
            e.action = "reboot";
            e.subject = "system";
            e.detail = line;
            e.timestamp = std::chrono::system_clock::now(); // Approximate.
            events.push_back(std::move(e));
            continue;
        }

        // Parse: username  tty  source  login_time - logout_time (duration)
        std::istringstream iss(line);
        std::string user, tty, source;
        iss >> user >> tty;
        if (user.empty()) continue;

        // Source might be absent for local logins.
        std::string rest_of_line;
        std::getline(iss, rest_of_line);

        TimelineEvent e;
        e.category = "login";
        e.action = (rest_of_line.find("still logged in") != std::string::npos)
                       ? "login"
                       : "session";
        e.subject = user;
        e.detail = "tty=" + tty + " " + rest_of_line;
        e.timestamp = std::chrono::system_clock::now(); // Simplified.
        events.push_back(std::move(e));
    }

    return events;
}

// ---------------------------------------------------------------------------
// Package collector (dpkg.log)
// ---------------------------------------------------------------------------

std::vector<TimelineEvent> Collectors::collect_packages() {
    std::vector<TimelineEvent> events;

    std::ifstream log("/var/log/dpkg.log");
    if (!log.is_open()) {
        // Try apt history.
        log.open("/var/log/apt/history.log");
    }
    if (!log.is_open()) return events;

    std::string line;
    while (std::getline(log, line)) {
        if (line.empty()) continue;

        // dpkg.log format: "2025-01-15 14:30:00 install package-name:arch version"
        // Or: "2025-01-15 14:30:00 remove package-name:arch version"
        if (line.find(" install ") != std::string::npos ||
            line.find(" remove ") != std::string::npos ||
            line.find(" upgrade ") != std::string::npos ||
            line.find(" purge ") != std::string::npos) {

            std::istringstream iss(line);
            std::string date, time_str, action, pkg;
            iss >> date >> time_str >> action >> pkg;

            // Strip architecture suffix.
            auto colon = pkg.find(':');
            if (colon != std::string::npos) {
                pkg = pkg.substr(0, colon);
            }

            TimelineEvent e;
            e.category = "package";
            e.action = action;
            e.subject = pkg;
            e.detail = line;
            e.timestamp = parse_time(date + " " + time_str);
            events.push_back(std::move(e));
        }
    }

    return events;
}

// ---------------------------------------------------------------------------
// File activity collector
// ---------------------------------------------------------------------------

std::vector<TimelineEvent> Collectors::collect_files(bool modified, bool accessed) {
    std::vector<TimelineEvent> events;
    std::string home = home_dir();

    // Recently-used.xbel (freedesktop standard).
    std::string xbel_path = home + "/.local/share/recently-used.xbel";
    std::ifstream xbel(xbel_path);
    if (xbel.is_open()) {
        std::string line;
        std::string current_href;
        while (std::getline(xbel, line)) {
            // Look for href="file:///..." and modified="..."
            auto href_pos = line.find("href=\"");
            if (href_pos != std::string::npos) {
                auto start = href_pos + 6;
                auto end = line.find('"', start);
                if (end != std::string::npos) {
                    current_href = line.substr(start, end - start);
                    // Decode file:// URI.
                    if (current_href.substr(0, 7) == "file://") {
                        current_href = current_href.substr(7);
                    }
                }
            }

            auto mod_pos = line.find("modified=\"");
            if (mod_pos != std::string::npos && !current_href.empty()) {
                auto start = mod_pos + 10;
                auto end = line.find('"', start);
                if (end != std::string::npos) {
                    std::string ts = line.substr(start, end - start);
                    TimelineEvent e;
                    e.category = "file";
                    e.action = "open";
                    e.subject = current_href;
                    e.timestamp = parse_time(ts);
                    events.push_back(std::move(e));
                    current_href.clear();
                }
            }
        }
    }

    // Find recently modified files in home directory.
    if (modified) {
        auto lines = run_lines(
            "find " + home + " -maxdepth 3 -type f -mmin -1440 "
            "-not -path '*/\\.*' -printf '%T@ %p\\n' 2>/dev/null | "
            "sort -rn | head -100");
        for (const auto& line : lines) {
            auto space = line.find(' ');
            if (space == std::string::npos) continue;
            std::string epoch_str = line.substr(0, space);
            std::string path = line.substr(space + 1);

            TimelineEvent e;
            e.category = "file";
            e.action = "modified";
            e.subject = path;
            e.timestamp = parse_time(epoch_str);
            events.push_back(std::move(e));
        }
    }

    // Find recently accessed files.
    if (accessed) {
        auto lines = run_lines(
            "find " + home + " -maxdepth 3 -type f -amin -1440 "
            "-not -path '*/\\.*' -printf '%A@ %p\\n' 2>/dev/null | "
            "sort -rn | head -100");
        for (const auto& line : lines) {
            auto space = line.find(' ');
            if (space == std::string::npos) continue;
            std::string epoch_str = line.substr(0, space);
            std::string path = line.substr(space + 1);

            TimelineEvent e;
            e.category = "file";
            e.action = "accessed";
            e.subject = path;
            e.timestamp = parse_time(epoch_str);
            events.push_back(std::move(e));
        }
    }

    return events;
}

// ---------------------------------------------------------------------------
// Service event collector (systemd journal)
// ---------------------------------------------------------------------------

std::vector<TimelineEvent> Collectors::collect_services() {
    std::vector<TimelineEvent> events;

    // Get recent service state changes from journal.
    auto lines = run_lines(
        "journalctl -u '*.service' --since '24 hours ago' "
        "--output=short-precise --no-pager 2>/dev/null | "
        "grep -E '(Started|Stopped|Failed|Reloaded)' | tail -200");

    for (const auto& line : lines) {
        TimelineEvent e;
        e.category = "service";

        if (line.find("Started") != std::string::npos) {
            e.action = "started";
        } else if (line.find("Stopped") != std::string::npos) {
            e.action = "stopped";
        } else if (line.find("Failed") != std::string::npos) {
            e.action = "failed";
        } else if (line.find("Reloaded") != std::string::npos) {
            e.action = "reloaded";
        } else {
            e.action = "event";
        }

        // Extract service name (appears after "Started/Stopped/Failed/Reloaded").
        e.subject = line;
        e.detail = line;

        // Try to parse the timestamp from the beginning of the line.
        // journalctl short-precise: "Jan 15 14:30:00.123456 hostname ..."
        if (line.size() > 15) {
            e.timestamp = parse_time(line.substr(0, 15));
        } else {
            e.timestamp = std::chrono::system_clock::now();
        }

        events.push_back(std::move(e));
    }

    return events;
}

// ---------------------------------------------------------------------------
// Command history collector
// ---------------------------------------------------------------------------

std::vector<TimelineEvent> Collectors::collect_commands() {
    std::vector<TimelineEvent> events;
    std::string home = home_dir();

    // Try zsh_history first (has timestamps), then bash_history.
    std::string zsh_hist = home + "/.zsh_history";
    std::ifstream zsh(zsh_hist);
    if (zsh.is_open()) {
        std::string line;
        while (std::getline(zsh, line)) {
            // zsh format: ": epoch:0;command"
            if (line.size() > 2 && line[0] == ':' && line[1] == ' ') {
                auto colon2 = line.find(':', 2);
                if (colon2 == std::string::npos) continue;
                std::string epoch_str = line.substr(2, colon2 - 2);
                auto semi = line.find(';', colon2);
                if (semi == std::string::npos) continue;
                std::string cmd = line.substr(semi + 1);

                TimelineEvent e;
                e.category = "command";
                e.action = "exec";
                e.subject = cmd;
                e.timestamp = parse_time(epoch_str);
                events.push_back(std::move(e));
            }
        }
        // Keep most recent 500.
        if (events.size() > 500) {
            events.erase(events.begin(), events.end() - 500);
        }
        return events;
    }

    // bash_history (no timestamps by default).
    std::string bash_hist = home + "/.bash_history";
    std::ifstream bash(bash_hist);
    if (bash.is_open()) {
        std::string line;
        int line_num = 0;
        while (std::getline(bash, line)) {
            if (line.empty() || line[0] == '#') continue;
            ++line_num;

            TimelineEvent e;
            e.category = "command";
            e.action = "exec";
            e.subject = line;
            e.detail = "line " + std::to_string(line_num);
            // No timestamp available; use epoch 0 to indicate unknown.
            e.timestamp = {};
            events.push_back(std::move(e));
        }
        // Keep most recent 500.
        if (events.size() > 500) {
            events.erase(events.begin(), events.end() - 500);
        }
    }

    return events;
}

// ---------------------------------------------------------------------------
// Git log collector
// ---------------------------------------------------------------------------

std::vector<TimelineEvent> Collectors::collect_git() {
    std::vector<TimelineEvent> events;
    std::string home = home_dir();

    // Find git repos under home (max depth 3).
    auto repo_dirs = run_lines(
        "find " + home + " -maxdepth 3 -name .git -type d 2>/dev/null | head -20");

    for (const auto& git_dir : repo_dirs) {
        std::string repo = fs::path(git_dir).parent_path().string();

        // Get recent commits.
        auto lines = run_lines(
            "git -C '" + repo + "' log --oneline --format='%H|%at|%s' "
            "--since='7 days ago' -n 50 2>/dev/null");

        for (const auto& line : lines) {
            auto p1 = line.find('|');
            auto p2 = line.find('|', p1 + 1);
            if (p1 == std::string::npos || p2 == std::string::npos) continue;

            std::string hash = line.substr(0, p1);
            std::string epoch_str = line.substr(p1 + 1, p2 - p1 - 1);
            std::string msg = line.substr(p2 + 1);

            TimelineEvent e;
            e.category = "git";
            e.action = "commit";
            e.subject = fs::path(repo).filename().string() + ": " + msg;
            e.detail = hash + " in " + repo;
            e.timestamp = parse_time(epoch_str);
            events.push_back(std::move(e));
        }
    }

    return events;
}

// ---------------------------------------------------------------------------
// Application usage collector
// ---------------------------------------------------------------------------

std::vector<TimelineEvent> Collectors::collect_apps() {
    std::vector<TimelineEvent> events;

    // Parse /proc for currently running GUI applications.
    auto lines = run_lines(
        "ps -eo pid,lstart,comm --sort=-start_time 2>/dev/null | head -50");

    for (size_t i = 1; i < lines.size(); ++i) { // Skip header.
        const auto& line = lines[i];
        if (line.empty()) continue;

        // Format: PID  Day Mon DD HH:MM:SS YYYY  COMMAND
        std::istringstream iss(line);
        std::string pid_str;
        iss >> pid_str;

        std::string dow, mon, day, time_str, year, cmd;
        iss >> dow >> mon >> day >> time_str >> year >> cmd;

        if (is_app_collector_noise(cmd)) continue;

        TimelineEvent e;
        e.category = "app";
        e.action = "running";
        e.subject = cmd;
        e.detail = "pid=" + pid_str;
        e.timestamp = parse_time(mon + " " + day + " " + time_str);
        events.push_back(std::move(e));
    }

    return events;
}

// ---------------------------------------------------------------------------
// Unified collector
// ---------------------------------------------------------------------------

std::vector<TimelineEvent> Collectors::collect_all() {
    std::vector<TimelineEvent> all;

    auto merge = [&](std::vector<TimelineEvent>&& src) {
        all.insert(all.end(),
                   std::make_move_iterator(src.begin()),
                   std::make_move_iterator(src.end()));
    };

    merge(collect_logins());
    merge(collect_packages());
    merge(collect_files(true, false));
    merge(collect_services());
    merge(collect_commands());
    merge(collect_git());
    merge(collect_apps());

    // Sort by timestamp, newest first.
    std::sort(all.begin(), all.end(),
              [](const TimelineEvent& a, const TimelineEvent& b) {
                  return a.timestamp > b.timestamp;
              });

    return all;
}

} // namespace straylight
