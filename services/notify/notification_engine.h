// services/notify/notification_engine.h
#pragma once

#include <straylight/result.h>
#include <straylight/error.h>

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace straylight {

/// Urgency levels matching freedesktop spec.
enum class Urgency : uint8_t {
    Low      = 0,
    Normal   = 1,
    Critical = 2,
};

/// Parse urgency from string.
Urgency parse_urgency(const std::string& str);

/// A notification action button.
struct NotifyAction {
    std::string key;
    std::string label;
};

/// A notification request.
struct Notification {
    uint32_t id = 0;
    std::string app_name;
    std::string summary;
    std::string body;
    std::string icon;
    Urgency urgency = Urgency::Normal;
    int timeout_ms = 5000;
    std::vector<NotifyAction> actions;
    std::chrono::system_clock::time_point timestamp;
    bool dismissed = false;
};

/// Rule action type.
enum class RuleAction : uint8_t {
    Show     = 0,
    Suppress = 1,
    Modify   = 2,
    Redirect = 3,
};

/// A notification rule: match on app/urgency/pattern and perform an action.
struct NotificationRule {
    uint32_t id = 0;
    std::string app_pattern;        // Regex or exact match on app_name
    std::string summary_pattern;    // Regex match on summary
    Urgency min_urgency = Urgency::Low;
    RuleAction action = RuleAction::Show;
    std::string modify_summary;     // Replacement summary for Modify action
    std::string redirect_target;    // Redirect to this app/channel
    bool enabled = true;
};

/// Do-not-disturb schedule.
struct DndSchedule {
    bool enabled = false;
    int start_hour = 22;    // 24-hour format
    int start_minute = 0;
    int end_hour = 7;
    int end_minute = 0;
};

/// Notification engine: manages notification lifecycle, rules, DND, history.
class NotificationEngine {
public:
    NotificationEngine();

    /// Submit a new notification. Returns the assigned notification ID.
    Result<uint32_t, SLError> notify(const std::string& app_name,
                                     const std::string& summary,
                                     const std::string& body,
                                     Urgency urgency,
                                     const std::vector<NotifyAction>& actions,
                                     const std::string& icon,
                                     int timeout_ms);

    /// Dismiss a notification by ID.
    Result<void, SLError> dismiss(uint32_t id);

    /// Invoke an action on a notification.
    Result<void, SLError> invoke_action(uint32_t id, const std::string& action_key);

    /// Get the notification history.
    std::vector<Notification> history(const std::string& app_filter = "") const;

    /// Clear all notification history.
    void clear_history();

    /// Get pending (undismissed) notifications.
    std::vector<Notification> pending() const;

    // -----------------------------------------------------------------------
    // Do-not-disturb
    // -----------------------------------------------------------------------

    void set_dnd(bool enabled);
    bool dnd_enabled() const;
    void set_dnd_schedule(const DndSchedule& schedule);
    DndSchedule get_dnd_schedule() const;

    // -----------------------------------------------------------------------
    // Rules
    // -----------------------------------------------------------------------

    Result<uint32_t, SLError> add_rule(const NotificationRule& rule);
    std::vector<NotificationRule> list_rules() const;
    Result<void, SLError> remove_rule(uint32_t id);

    // -----------------------------------------------------------------------
    // Persistence
    // -----------------------------------------------------------------------

    Result<void, SLError> load_history(const std::string& path);
    Result<void, SLError> save_history(const std::string& path) const;
    Result<void, SLError> load_rules(const std::string& path);
    Result<void, SLError> save_rules(const std::string& path) const;

    /// Set the callback for displaying notifications.
    using DisplayCallback = std::function<void(const Notification&)>;
    void set_display_callback(DisplayCallback cb);

    /// Set the callback for playing notification sounds.
    using SoundCallback = std::function<void(Urgency)>;
    void set_sound_callback(SoundCallback cb);

    /// Set the callback for action invocations (to send D-Bus signals back).
    using ActionCallback = std::function<void(uint32_t id, const std::string& action_key)>;
    void set_action_callback(ActionCallback cb);

private:
    /// Apply rules to a notification. Returns true if notification should be shown.
    bool apply_rules(Notification& notif) const;

    /// Check if DND is currently active.
    bool is_dnd_active() const;

    /// Collapse duplicate notifications from same app.
    bool try_collapse(const Notification& notif);

    /// Play notification sound.
    void play_sound(Urgency urgency) const;

    /// Trim history to max size.
    void trim_history();

    mutable std::mutex mutex_;
    std::deque<Notification> history_;
    std::vector<NotificationRule> rules_;

    uint32_t next_notif_id_ = 1;
    uint32_t next_rule_id_ = 1;

    bool dnd_ = false;
    DndSchedule dnd_schedule_;

    static constexpr size_t kMaxHistory = 1000;

    DisplayCallback display_cb_;
    SoundCallback sound_cb_;
    ActionCallback action_cb_;
};

} // namespace straylight
