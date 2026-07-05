/**
 * StrayLight Prediction Engine — Usage pattern learning and next-app prediction.
 *
 * Reads timeline data from ~/.local/share/straylight/timeline.db (via sqlite3 CLI),
 * builds a Markov chain: P(next_app | current_app, time_bucket), and predicts
 * which apps the user is most likely to launch next.
 */
#pragma once

#include "straylight/result.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <mutex>
#include <numeric>
#include <string>
#include <vector>
#include <unistd.h>

namespace straylight::predict {

// ─── Types ──────────────────────────────────────────────────────────────────

struct UsagePattern {
    std::string app_name;
    std::vector<int> typical_launch_hours;   // 0-23
    std::vector<int> typical_launch_days;    // 0=Sun..6=Sat
    std::string preceded_by;                  // which app was used before
    int total_launches = 0;
    double avg_session_seconds = 0.0;
};

struct Prediction {
    std::string app_name;
    double probability = 0.0;
    std::string reason;
};

// Time is bucketed into 4-hour windows: 0-3, 4-7, 8-11, 12-15, 16-19, 20-23
inline int time_bucket(int hour) { return hour / 4; }

// ─── Markov Model ───────────────────────────────────────────────────────────

/** Key for the Markov transition table: (current_app, time_bucket, day_of_week) */
struct MarkovKey {
    std::string current_app;
    int time_bucket = 0;
    int day_of_week = 0;

    bool operator<(const MarkovKey& o) const {
        if (current_app != o.current_app) return current_app < o.current_app;
        if (time_bucket != o.time_bucket) return time_bucket < o.time_bucket;
        return day_of_week < o.day_of_week;
    }
};

class PredictionEngine {
public:
    PredictionEngine() = default;

    /** Train the model from timeline data. Shells out to sqlite3. */
    Result<int, std::string> train() {
        std::lock_guard<std::mutex> lock(mtx_);

        std::string db_path = get_timeline_db_path();
        if (db_path.empty()) {
            return train_from_procfs();
        }

        // Query all app launch events from the timeline database
        // Expected schema: timestamp (unix), app_name, event_type, duration_seconds
        std::string query =
            "SELECT timestamp, app_name, duration_seconds "
            "FROM timeline "
            "WHERE event_type = 'app_launch' "
            "ORDER BY timestamp ASC;";

        std::string cmd = "sqlite3 -separator '|' '" + db_path + "' \"" + query + "\" 2>/dev/null";
        std::string output = exec_cmd(cmd);

        if (output.empty()) {
            // Try alternate schema: some installations use 'events' table
            query = "SELECT timestamp, app_name, duration "
                    "FROM events "
                    "WHERE type = 'launch' "
                    "ORDER BY timestamp ASC;";
            cmd = "sqlite3 -separator '|' '" + db_path + "' \"" + query + "\" 2>/dev/null";
            output = exec_cmd(cmd);
        }

        if (output.empty()) {
            // Current straylight-timeline schema: category/action/subject/detail/timestamp.
            query = "SELECT timestamp, subject, 0 "
                    "FROM events "
                    "WHERE category = 'app' "
                    "ORDER BY timestamp ASC;";
            cmd = "sqlite3 -separator '|' '" + db_path + "' \"" + query + "\" 2>/dev/null";
            output = exec_cmd(cmd);
        }

        // Parse results and build model
        transitions_.clear();
        frequency_.clear();
        patterns_.clear();

        struct LaunchEvent {
            time_t timestamp;
            std::string app_name;
            double duration;
        };

        std::vector<LaunchEvent> events;

        // Parse pipe-separated output line by line
        size_t pos = 0;
        while (pos < output.size()) {
            auto nl = output.find('\n', pos);
            if (nl == std::string::npos) nl = output.size();
            std::string line = output.substr(pos, nl - pos);
            pos = nl + 1;

            if (line.empty()) continue;

            // Parse: timestamp|app_name|duration
            auto p1 = line.find('|');
            if (p1 == std::string::npos) continue;
            auto p2 = line.find('|', p1 + 1);
            if (p2 == std::string::npos) continue;

            LaunchEvent ev;
            try {
                ev.timestamp = std::stol(line.substr(0, p1));
                ev.app_name = line.substr(p1 + 1, p2 - p1 - 1);
                ev.duration = std::stod(line.substr(p2 + 1));
            } catch (...) {
                continue;
            }

            if (ev.app_name.empty() || is_procfs_noise_app(ev.app_name)) continue;
            events.push_back(ev);
        }

        if (events.empty()) {
            // Generate synthetic training data from /proc if no timeline exists
            return train_from_procfs();
        }

        // Build Markov transitions and usage patterns
        for (size_t i = 0; i < events.size(); ++i) {
            const auto& ev = events[i];

            struct tm tm_buf{};
            localtime_r(&ev.timestamp, &tm_buf);
            int hour = tm_buf.tm_hour;
            int dow = tm_buf.tm_wday;
            int tb = time_bucket(hour);

            // Frequency count
            frequency_[ev.app_name]++;

            // Update pattern
            auto& pat = patterns_[ev.app_name];
            pat.app_name = ev.app_name;
            pat.total_launches++;
            if (ev.duration > 0) {
                // Running average
                pat.avg_session_seconds =
                    pat.avg_session_seconds +
                    (ev.duration - pat.avg_session_seconds) / pat.total_launches;
            }

            // Track launch hours and days
            if (std::find(pat.typical_launch_hours.begin(),
                          pat.typical_launch_hours.end(), hour) ==
                pat.typical_launch_hours.end()) {
                pat.typical_launch_hours.push_back(hour);
            }
            if (std::find(pat.typical_launch_days.begin(),
                          pat.typical_launch_days.end(), dow) ==
                pat.typical_launch_days.end()) {
                pat.typical_launch_days.push_back(dow);
            }

            // Transition: previous app -> this app
            if (i > 0) {
                const auto& prev = events[i - 1];
                pat.preceded_by = prev.app_name;

                MarkovKey key{prev.app_name, tb, dow};
                transitions_[key][ev.app_name]++;

                // Also add a time-only key (empty app) for time-based predictions
                MarkovKey time_key{"", tb, dow};
                transitions_[time_key][ev.app_name]++;
            }
        }

        last_train_time_ = std::chrono::steady_clock::now();
        total_events_ = static_cast<int>(events.size());
        return Result<int, std::string>::ok(total_events_);
    }

    /** Predict the top-N most likely next apps. */
    std::vector<Prediction> predict_next_apps(
        int top_n,
        const std::string& current_app = "",
        const std::vector<std::string>& running_apps = {}) const {

        std::lock_guard<std::mutex> lock(mtx_);

        auto now = std::chrono::system_clock::now();
        auto tt = std::chrono::system_clock::to_time_t(now);
        struct tm tm_buf{};
        localtime_r(&tt, &tm_buf);
        int tb = time_bucket(tm_buf.tm_hour);
        int dow = tm_buf.tm_wday;

        std::map<std::string, double> scores;

        // Factor 1: Markov transition from current app
        if (!current_app.empty()) {
            MarkovKey key{current_app, tb, dow};
            auto it = transitions_.find(key);
            if (it != transitions_.end()) {
                int total = 0;
                for (const auto& [_, count] : it->second) total += count;
                for (const auto& [app, count] : it->second) {
                    double prob = static_cast<double>(count) / total;
                    scores[app] += prob * 0.5;  // 50% weight for transition
                }
            }

            // Also check without day-of-week specificity
            MarkovKey relaxed_key{current_app, tb, -1};
            // Aggregate across all days
            for (int d = 0; d < 7; ++d) {
                MarkovKey dk{current_app, tb, d};
                auto dit = transitions_.find(dk);
                if (dit != transitions_.end()) {
                    int total = 0;
                    for (const auto& [_, count] : dit->second) total += count;
                    for (const auto& [app, count] : dit->second) {
                        double prob = static_cast<double>(count) / total;
                        scores[app] += prob * 0.1;  // 10% weight for relaxed transition
                    }
                }
            }
        }

        // Factor 2: Time-of-day patterns
        MarkovKey time_key{"", tb, dow};
        auto time_it = transitions_.find(time_key);
        if (time_it != transitions_.end()) {
            int total = 0;
            for (const auto& [_, count] : time_it->second) total += count;
            for (const auto& [app, count] : time_it->second) {
                double prob = static_cast<double>(count) / total;
                scores[app] += prob * 0.25;  // 25% weight for time pattern
            }
        }

        // Factor 3: Overall frequency
        if (!frequency_.empty()) {
            int total_freq = 0;
            for (const auto& [_, count] : frequency_) total_freq += count;
            for (const auto& [app, count] : frequency_) {
                double prob = static_cast<double>(count) / total_freq;
                scores[app] += prob * 0.15;  // 15% weight for frequency
            }
        }

        // Penalize already-running apps (they don't need preloading)
        for (const auto& running : running_apps) {
            scores[running] *= 0.1;
        }

        // Remove the current app from predictions
        if (!current_app.empty()) {
            scores.erase(current_app);
        }

        // Sort by score
        std::vector<Prediction> predictions;
        for (const auto& [app, score] : scores) {
            if (score > 0.01) {
                Prediction p;
                p.app_name = app;
                p.probability = score;

                // Build reason string
                if (!current_app.empty()) {
                    MarkovKey key{current_app, tb, dow};
                    auto it = transitions_.find(key);
                    if (it != transitions_.end() &&
                        it->second.find(app) != it->second.end()) {
                        p.reason = "often follows " + current_app;
                    }
                }
                if (p.reason.empty()) {
                    auto pat_it = patterns_.find(app);
                    if (pat_it != patterns_.end()) {
                        for (int h : pat_it->second.typical_launch_hours) {
                            if (time_bucket(h) == tb) {
                                p.reason = "typically used at this time";
                                break;
                            }
                        }
                    }
                }
                if (p.reason.empty()) {
                    p.reason = "frequently used";
                }

                predictions.push_back(p);
            }
        }

        std::sort(predictions.begin(), predictions.end(),
                  [](const Prediction& a, const Prediction& b) {
                      return a.probability > b.probability;
                  });

        if (static_cast<int>(predictions.size()) > top_n) {
            predictions.resize(top_n);
        }

        // Normalize probabilities to sum to 1.0
        double total = 0.0;
        for (const auto& p : predictions) total += p.probability;
        if (total > 0.0) {
            for (auto& p : predictions) p.probability /= total;
        }

        return predictions;
    }

    /** Get usage patterns for all known apps. */
    std::vector<UsagePattern> get_patterns() const {
        std::lock_guard<std::mutex> lock(mtx_);
        std::vector<UsagePattern> result;
        for (const auto& [_, pat] : patterns_) {
            result.push_back(pat);
        }
        std::sort(result.begin(), result.end(),
                  [](const UsagePattern& a, const UsagePattern& b) {
                      return a.total_launches > b.total_launches;
                  });
        return result;
    }

    /** Check if model needs retraining (>1 hour since last train). */
    bool needs_retrain() const {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::minutes>(
            now - last_train_time_).count();
        return elapsed >= 60;
    }

    /** Number of events in the model. */
    int total_events() const { return total_events_; }

    /** Number of known apps. */
    int app_count() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return static_cast<int>(patterns_.size());
    }

private:
    mutable std::mutex mtx_;

    // Markov transition table: key -> {next_app -> count}
    std::map<MarkovKey, std::map<std::string, int>> transitions_;

    // Overall app frequency
    std::map<std::string, int> frequency_;

    // Per-app usage patterns
    std::map<std::string, UsagePattern> patterns_;

    std::chrono::steady_clock::time_point last_train_time_{};
    int total_events_ = 0;

    std::string get_timeline_db_path() const {
        std::vector<std::string> candidates;
        if (const char* explicit_path = getenv("STRAYLIGHT_TIMELINE_DB")) {
            if (*explicit_path) candidates.push_back(explicit_path);
        }
        if (const char* state_home = getenv("XDG_STATE_HOME")) {
            candidates.push_back(std::string(state_home) +
                                 "/straylight/timeline.db");
        }
        if (const char* home = getenv("HOME")) {
            candidates.push_back(std::string(home) +
                                 "/.local/share/straylight/timeline.db");
            candidates.push_back(std::string(home) +
                                 "/.config/straylight/timeline.db");
        }
        candidates.push_back("/var/lib/straylight/timeline.db");

        for (const auto& path : candidates) {
            if (access(path.c_str(), R_OK) == 0) return path;
        }
        return "";
    }

    static std::string exec_cmd(const std::string& cmd) {
        std::string result;
        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) return "";
        char buf[4096];
        while (fgets(buf, sizeof(buf), pipe) != nullptr) {
            result += buf;
        }
        pclose(pipe);
        return result;
    }

    static bool is_procfs_noise_app(const std::string& app) {
        static const std::vector<std::string> exact = {
            "agetty", "avahi-daemon", "awk", "bash", "cat", "cc1plus",
            "cmake", "c++", "dbus-daemon", "gnome-keyring-d", "grep", "head",
            "install", "journalctl", "ld", "ninja", "nm-dispatcher",
            "pipewire", "pipewire-pulse", "ps", "sed", "sh", "sleep",
            "sort", "sshd-session", "sudo", "systemctl", "tail", "udisksd",
            "uniq", "upowerd", "watchdogd", "wireplumber", "wpa_supplicant",
            "xdg-desktop-por", "xdg-document-po", "xdg-permission-", "zsh"
        };
        if (app.empty() ||
            std::find(exact.begin(), exact.end(), app) != exact.end()) {
            return true;
        }
        if (app.front() == '(' || app.find(' ') != std::string::npos) {
            return true;
        }
        static const std::vector<std::string> prefixes = {
            "irq/", "kworker", "nv_", "rcu_", "straylight-", "systemd-", "usb-"
        };
        for (const auto& prefix : prefixes) {
            if (app.rfind(prefix, 0) == 0) return true;
        }
        return false;
    }

    /** Fallback: build initial model from /proc filesystem.
     *  This gives us something to work with before timeline data exists. */
    Result<int, std::string> train_from_procfs() {
        // Read currently running processes to seed the model
        std::string output = exec_cmd(
            "ps -eo comm --no-headers 2>/dev/null | sort | uniq -c | sort -rn | awk 'NR<=30 {print}'");

        if (output.empty()) {
            return Result<int, std::string>::ok(0);
        }

        auto now = std::chrono::system_clock::now();
        auto tt = std::chrono::system_clock::to_time_t(now);
        struct tm tm_buf{};
        localtime_r(&tt, &tm_buf);
        int hour = tm_buf.tm_hour;
        int dow = tm_buf.tm_wday;
        int tb = time_bucket(hour);

        int count = 0;
        size_t pos = 0;
        std::string prev_app;

        while (pos < output.size()) {
            auto nl = output.find('\n', pos);
            if (nl == std::string::npos) nl = output.size();
            std::string line = output.substr(pos, nl - pos);
            pos = nl + 1;

            // Parse "   count name"
            auto first_nonspace = line.find_first_not_of(" \t");
            if (first_nonspace == std::string::npos) continue;
            auto space = line.find(' ', first_nonspace);
            if (space == std::string::npos) continue;
            auto name_start = line.find_first_not_of(" \t", space);
            if (name_start == std::string::npos) continue;

            int freq = 0;
            try { freq = std::stoi(line.substr(first_nonspace)); } catch (...) { continue; }

            std::string app = line.substr(name_start);
            // Trim trailing whitespace
            while (!app.empty() && (app.back() == ' ' || app.back() == '\n'))
                app.pop_back();

            if (is_procfs_noise_app(app)) continue;

            frequency_[app] = freq;

            auto& pat = patterns_[app];
            pat.app_name = app;
            pat.total_launches = freq;
            pat.typical_launch_hours.push_back(hour);
            pat.typical_launch_days.push_back(dow);

            // Create a simple transition chain
            if (!prev_app.empty()) {
                MarkovKey key{prev_app, tb, dow};
                transitions_[key][app] += freq;

                MarkovKey time_key{"", tb, dow};
                transitions_[time_key][app] += freq;

                pat.preceded_by = prev_app;
            }

            prev_app = app;
            ++count;
        }

        total_events_ = count;
        last_train_time_ = std::chrono::steady_clock::now();
        return Result<int, std::string>::ok(count);
    }
};

} // namespace straylight::predict
