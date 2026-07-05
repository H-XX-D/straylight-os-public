// services/notify/notification_engine.cpp
#include "notification_engine.h"

#include <straylight/log.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

namespace straylight {

// ---------------------------------------------------------------------------
// Urgency parsing
// ---------------------------------------------------------------------------

Urgency parse_urgency(const std::string& str) {
    if (str == "low")      return Urgency::Low;
    if (str == "critical") return Urgency::Critical;
    return Urgency::Normal;
}

static const char* urgency_str(Urgency u) {
    switch (u) {
        case Urgency::Low:      return "low";
        case Urgency::Normal:   return "normal";
        case Urgency::Critical: return "critical";
    }
    return "normal";
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

NotificationEngine::NotificationEngine() = default;

// ---------------------------------------------------------------------------
// Core notify
// ---------------------------------------------------------------------------

Result<uint32_t, SLError> NotificationEngine::notify(
    const std::string& app_name,
    const std::string& summary,
    const std::string& body,
    Urgency urgency,
    const std::vector<NotifyAction>& actions,
    const std::string& icon,
    int timeout_ms) {

    std::lock_guard<std::mutex> lock(mutex_);

    Notification notif;
    notif.id = next_notif_id_++;
    notif.app_name = app_name;
    notif.summary = summary;
    notif.body = body;
    notif.urgency = urgency;
    notif.actions = actions;
    notif.icon = icon;
    notif.timeout_ms = (timeout_ms > 0) ? timeout_ms : 5000;
    notif.timestamp = std::chrono::system_clock::now();

    // Apply rules.
    bool should_show = apply_rules(notif);

    // Check DND.
    if (is_dnd_active() && urgency != Urgency::Critical) {
        should_show = false;
    }

    // Try to collapse with existing notification from same app.
    if (should_show && try_collapse(notif)) {
        // Collapsed into existing — still add to history.
        history_.push_back(notif);
        trim_history();
        return Result<uint32_t, SLError>::ok(notif.id);
    }

    // Add to history.
    history_.push_back(notif);
    trim_history();

    // Display if allowed.
    if (should_show) {
        play_sound(urgency);
        if (display_cb_) {
            display_cb_(notif);
        }
        SL_INFO("notify: [{}] {} — {}", app_name, summary,
                body.substr(0, 50));
    } else {
        SL_DEBUG("notify: suppressed notification from {} (DND or rule)", app_name);
    }

    return Result<uint32_t, SLError>::ok(notif.id);
}

// ---------------------------------------------------------------------------
// Dismiss / Action
// ---------------------------------------------------------------------------

Result<void, SLError> NotificationEngine::dismiss(uint32_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& n : history_) {
        if (n.id == id) {
            n.dismissed = true;
            return Result<void, SLError>::ok();
        }
    }
    return Result<void, SLError>::error(
        {SLErrorCode::NotFound, "Notification not found: " + std::to_string(id)});
}

Result<void, SLError> NotificationEngine::invoke_action(uint32_t id,
                                                         const std::string& action_key) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& n : history_) {
        if (n.id == id) {
            bool found = false;
            for (const auto& a : n.actions) {
                if (a.key == action_key) { found = true; break; }
            }
            if (!found) {
                return Result<void, SLError>::error(
                    {SLErrorCode::NotFound, "Action not found: " + action_key});
            }
            if (action_cb_) {
                action_cb_(id, action_key);
            }
            n.dismissed = true;
            return Result<void, SLError>::ok();
        }
    }
    return Result<void, SLError>::error(
        {SLErrorCode::NotFound, "Notification not found: " + std::to_string(id)});
}

// ---------------------------------------------------------------------------
// History
// ---------------------------------------------------------------------------

std::vector<Notification> NotificationEngine::history(const std::string& app_filter) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (app_filter.empty()) {
        return std::vector<Notification>(history_.begin(), history_.end());
    }

    std::vector<Notification> filtered;
    for (const auto& n : history_) {
        if (n.app_name.find(app_filter) != std::string::npos) {
            filtered.push_back(n);
        }
    }
    return filtered;
}

void NotificationEngine::clear_history() {
    std::lock_guard<std::mutex> lock(mutex_);
    history_.clear();
}

std::vector<Notification> NotificationEngine::pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Notification> result;
    for (const auto& n : history_) {
        if (!n.dismissed) result.push_back(n);
    }
    return result;
}

// ---------------------------------------------------------------------------
// DND
// ---------------------------------------------------------------------------

void NotificationEngine::set_dnd(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    dnd_ = enabled;
    SL_INFO("notify: do-not-disturb {}", enabled ? "enabled" : "disabled");
}

bool NotificationEngine::dnd_enabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dnd_;
}

void NotificationEngine::set_dnd_schedule(const DndSchedule& schedule) {
    std::lock_guard<std::mutex> lock(mutex_);
    dnd_schedule_ = schedule;
}

DndSchedule NotificationEngine::get_dnd_schedule() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dnd_schedule_;
}

bool NotificationEngine::is_dnd_active() const {
    if (dnd_) return true;

    if (dnd_schedule_.enabled) {
        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);
        std::tm tm_now;
        localtime_r(&time_t_now, &tm_now);

        int current_minutes = tm_now.tm_hour * 60 + tm_now.tm_min;
        int start_minutes = dnd_schedule_.start_hour * 60 + dnd_schedule_.start_minute;
        int end_minutes = dnd_schedule_.end_hour * 60 + dnd_schedule_.end_minute;

        if (start_minutes <= end_minutes) {
            // Same-day range (e.g., 09:00 to 17:00).
            return current_minutes >= start_minutes && current_minutes < end_minutes;
        } else {
            // Overnight range (e.g., 22:00 to 07:00).
            return current_minutes >= start_minutes || current_minutes < end_minutes;
        }
    }

    return false;
}

// ---------------------------------------------------------------------------
// Rules
// ---------------------------------------------------------------------------

Result<uint32_t, SLError> NotificationEngine::add_rule(const NotificationRule& rule) {
    std::lock_guard<std::mutex> lock(mutex_);
    NotificationRule r = rule;
    r.id = next_rule_id_++;
    rules_.push_back(r);
    SL_INFO("notify: added rule #{} (app='{}', action={})",
            r.id, r.app_pattern, static_cast<int>(r.action));
    return Result<uint32_t, SLError>::ok(r.id);
}

std::vector<NotificationRule> NotificationEngine::list_rules() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return rules_;
}

Result<void, SLError> NotificationEngine::remove_rule(uint32_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(rules_.begin(), rules_.end(),
                           [id](const NotificationRule& r) { return r.id == id; });
    if (it == rules_.end()) {
        return Result<void, SLError>::error(
            {SLErrorCode::NotFound, "Rule not found: " + std::to_string(id)});
    }
    rules_.erase(it);
    return Result<void, SLError>::ok();
}

bool NotificationEngine::apply_rules(Notification& notif) const {
    for (const auto& rule : rules_) {
        if (!rule.enabled) continue;

        // Match app name.
        if (!rule.app_pattern.empty()) {
            try {
                std::regex re(rule.app_pattern, std::regex::icase);
                if (!std::regex_search(notif.app_name, re)) continue;
            } catch (...) {
                if (notif.app_name.find(rule.app_pattern) == std::string::npos) continue;
            }
        }

        // Match summary.
        if (!rule.summary_pattern.empty()) {
            try {
                std::regex re(rule.summary_pattern, std::regex::icase);
                if (!std::regex_search(notif.summary, re)) continue;
            } catch (...) {
                if (notif.summary.find(rule.summary_pattern) == std::string::npos) continue;
            }
        }

        // Match urgency.
        if (notif.urgency < rule.min_urgency) continue;

        // Rule matched — apply action.
        switch (rule.action) {
            case RuleAction::Suppress:
                return false;
            case RuleAction::Modify:
                if (!rule.modify_summary.empty()) {
                    notif.summary = rule.modify_summary;
                }
                return true;
            case RuleAction::Redirect:
                notif.app_name = rule.redirect_target;
                return true;
            case RuleAction::Show:
                return true;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Collapse duplicates
// ---------------------------------------------------------------------------

bool NotificationEngine::try_collapse(const Notification& notif) {
    // Look for a recent undismissed notification from the same app with the same summary.
    for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
        if (it->dismissed) continue;
        if (it->app_name == notif.app_name && it->summary == notif.summary) {
            // Update the existing notification's body and timestamp.
            it->body = notif.body;
            it->timestamp = notif.timestamp;
            SL_DEBUG("notify: collapsed duplicate from {}", notif.app_name);
            return true;
        }
        // Only look back through recent notifications.
        auto age = notif.timestamp - it->timestamp;
        if (age > std::chrono::seconds(30)) break;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Sound
// ---------------------------------------------------------------------------

void NotificationEngine::play_sound(Urgency urgency) const {
    if (sound_cb_) {
        sound_cb_(urgency);
        return;
    }

    // Default: use paplay or aplay with system notification sounds.
    std::string sound_file;
    switch (urgency) {
        case Urgency::Critical:
            sound_file = "/usr/share/sounds/straylight/critical.wav";
            break;
        case Urgency::Low:
            sound_file = "/usr/share/sounds/straylight/subtle.wav";
            break;
        default:
            sound_file = "/usr/share/sounds/straylight/notification.wav";
            break;
    }

    if (!fs::exists(sound_file)) {
        // Fallback to freedesktop sounds.
        sound_file = "/usr/share/sounds/freedesktop/stereo/message.oga";
    }

    if (fs::exists(sound_file)) {
        std::string cmd = "paplay '" + sound_file + "' 2>/dev/null &";
        int rc = std::system(cmd.c_str());
        if (rc != 0) {
            // Try aplay as fallback.
            cmd = "aplay -q '" + sound_file + "' 2>/dev/null &";
            std::system(cmd.c_str());
        }
    }
}

// ---------------------------------------------------------------------------
// Trim
// ---------------------------------------------------------------------------

void NotificationEngine::trim_history() {
    while (history_.size() > kMaxHistory) {
        history_.pop_front();
    }
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

Result<void, SLError> NotificationEngine::save_history(const std::string& path) const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);

    std::ofstream ofs(path);
    if (!ofs) {
        return Result<void, SLError>::error(
            {SLErrorCode::IOError, "Cannot write history: " + path});
    }

    ofs << "[\n";
    bool first = true;
    for (const auto& n : history_) {
        if (!first) ofs << ",\n";
        first = false;

        auto epoch = std::chrono::duration_cast<std::chrono::seconds>(
            n.timestamp.time_since_epoch()).count();

        ofs << "  {\n"
            << "    \"id\": " << n.id << ",\n"
            << "    \"app\": \"" << n.app_name << "\",\n"
            << "    \"summary\": \"" << n.summary << "\",\n"
            << "    \"body\": \"" << n.body << "\",\n"
            << "    \"urgency\": \"" << urgency_str(n.urgency) << "\",\n"
            << "    \"timestamp\": " << epoch << ",\n"
            << "    \"dismissed\": " << (n.dismissed ? "true" : "false") << "\n"
            << "  }";
    }
    ofs << "\n]\n";

    return Result<void, SLError>::ok();
}

Result<void, SLError> NotificationEngine::load_history(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::ifstream ifs(path);
    if (!ifs) {
        // Not an error if file doesn't exist — start fresh.
        return Result<void, SLError>::ok();
    }

    std::string content((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());

    // Simple incremental JSON parsing of the array.
    history_.clear();

    size_t pos = 0;
    while (true) {
        auto obj_start = content.find('{', pos);
        if (obj_start == std::string::npos) break;
        auto obj_end = content.find('}', obj_start);
        if (obj_end == std::string::npos) break;

        std::string obj = content.substr(obj_start, obj_end - obj_start + 1);
        pos = obj_end + 1;

        Notification n;

        // Parse individual fields.
        auto extract_string = [&](const std::string& key) -> std::string {
            auto kpos = obj.find("\"" + key + "\"");
            if (kpos == std::string::npos) return "";
            auto colon = obj.find(':', kpos);
            auto q1 = obj.find('"', colon + 1);
            auto q2 = obj.find('"', q1 + 1);
            if (q1 == std::string::npos || q2 == std::string::npos) return "";
            return obj.substr(q1 + 1, q2 - q1 - 1);
        };

        auto extract_int = [&](const std::string& key) -> int64_t {
            auto kpos = obj.find("\"" + key + "\"");
            if (kpos == std::string::npos) return 0;
            auto colon = obj.find(':', kpos);
            if (colon == std::string::npos) return 0;
            return std::strtoll(obj.c_str() + colon + 1, nullptr, 10);
        };

        auto extract_bool = [&](const std::string& key) -> bool {
            auto kpos = obj.find("\"" + key + "\"");
            if (kpos == std::string::npos) return false;
            return obj.find("true", kpos) != std::string::npos &&
                   obj.find("true", kpos) < obj.find('\n', kpos);
        };

        n.id = static_cast<uint32_t>(extract_int("id"));
        n.app_name = extract_string("app");
        n.summary = extract_string("summary");
        n.body = extract_string("body");
        n.urgency = parse_urgency(extract_string("urgency"));
        n.dismissed = extract_bool("dismissed");

        int64_t epoch = extract_int("timestamp");
        n.timestamp = std::chrono::system_clock::from_time_t(static_cast<time_t>(epoch));

        history_.push_back(n);

        if (n.id >= next_notif_id_) {
            next_notif_id_ = n.id + 1;
        }
    }

    return Result<void, SLError>::ok();
}

Result<void, SLError> NotificationEngine::save_rules(const std::string& path) const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);

    std::ofstream ofs(path);
    if (!ofs) {
        return Result<void, SLError>::error(
            {SLErrorCode::IOError, "Cannot write rules: " + path});
    }

    static const char* action_names[] = {"show", "suppress", "modify", "redirect"};

    ofs << "[\n";
    bool first = true;
    for (const auto& r : rules_) {
        if (!first) ofs << ",\n";
        first = false;

        ofs << "  {\n"
            << "    \"id\": " << r.id << ",\n"
            << "    \"app_pattern\": \"" << r.app_pattern << "\",\n"
            << "    \"summary_pattern\": \"" << r.summary_pattern << "\",\n"
            << "    \"min_urgency\": \"" << urgency_str(r.min_urgency) << "\",\n"
            << "    \"action\": \"" << action_names[static_cast<int>(r.action)] << "\",\n"
            << "    \"modify_summary\": \"" << r.modify_summary << "\",\n"
            << "    \"redirect_target\": \"" << r.redirect_target << "\",\n"
            << "    \"enabled\": " << (r.enabled ? "true" : "false") << "\n"
            << "  }";
    }
    ofs << "\n]\n";

    return Result<void, SLError>::ok();
}

Result<void, SLError> NotificationEngine::load_rules(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::ifstream ifs(path);
    if (!ifs) return Result<void, SLError>::ok();

    std::string content((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());

    rules_.clear();
    size_t pos = 0;
    while (true) {
        auto obj_start = content.find('{', pos);
        if (obj_start == std::string::npos) break;
        auto obj_end = content.find('}', obj_start);
        if (obj_end == std::string::npos) break;

        std::string obj = content.substr(obj_start, obj_end - obj_start + 1);
        pos = obj_end + 1;

        auto extract_string = [&](const std::string& key) -> std::string {
            auto kpos = obj.find("\"" + key + "\"");
            if (kpos == std::string::npos) return "";
            auto colon = obj.find(':', kpos);
            auto q1 = obj.find('"', colon + 1);
            auto q2 = obj.find('"', q1 + 1);
            if (q1 == std::string::npos || q2 == std::string::npos) return "";
            return obj.substr(q1 + 1, q2 - q1 - 1);
        };

        auto extract_int = [&](const std::string& key) -> int64_t {
            auto kpos = obj.find("\"" + key + "\"");
            if (kpos == std::string::npos) return 0;
            auto colon = obj.find(':', kpos);
            return std::strtoll(obj.c_str() + colon + 1, nullptr, 10);
        };

        NotificationRule r;
        r.id = static_cast<uint32_t>(extract_int("id"));
        r.app_pattern = extract_string("app_pattern");
        r.summary_pattern = extract_string("summary_pattern");
        r.min_urgency = parse_urgency(extract_string("min_urgency"));
        r.modify_summary = extract_string("modify_summary");
        r.redirect_target = extract_string("redirect_target");

        std::string action_str = extract_string("action");
        if (action_str == "suppress") r.action = RuleAction::Suppress;
        else if (action_str == "modify") r.action = RuleAction::Modify;
        else if (action_str == "redirect") r.action = RuleAction::Redirect;
        else r.action = RuleAction::Show;

        auto enabled_pos = obj.find("\"enabled\"");
        r.enabled = (enabled_pos == std::string::npos) ||
                    (obj.find("true", enabled_pos) != std::string::npos);

        rules_.push_back(r);
        if (r.id >= next_rule_id_) next_rule_id_ = r.id + 1;
    }

    return Result<void, SLError>::ok();
}

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

void NotificationEngine::set_display_callback(DisplayCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    display_cb_ = std::move(cb);
}

void NotificationEngine::set_sound_callback(SoundCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    sound_cb_ = std::move(cb);
}

void NotificationEngine::set_action_callback(ActionCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    action_cb_ = std::move(cb);
}

} // namespace straylight
