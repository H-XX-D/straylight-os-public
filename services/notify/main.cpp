// services/notify/main.cpp
// straylight-notify — Notification daemon implementing freedesktop notification protocol.
#include "notification_engine.h"
#include "dbus_bridge.h"

#include <straylight/common.h>
#include <straylight/daemon.h>

#include <chrono>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>

namespace straylight {

static constexpr const char* kHistoryPath = "/var/lib/straylight/notify/history.json";
static constexpr const char* kRulesPath   = "/var/lib/straylight/notify/rules.json";
static constexpr const char* kSocketPath  = "/run/straylight/notify.sock";

/// Notification daemon: listens on D-Bus (org.freedesktop.Notifications) and
/// on a local IPC socket for the straylight-notify-cli.
class NotifyDaemon : public DaemonBase {
public:
    Result<void, SLError> init(const Config& cfg) override {
        // Load configuration.
        bool dnd_default = cfg.get<bool>("dnd.default", false);
        engine_.set_dnd(dnd_default);

        if (cfg.has("dnd.schedule.enabled")) {
            DndSchedule sched;
            sched.enabled = cfg.get<bool>("dnd.schedule.enabled", false);
            sched.start_hour = cfg.get<int>("dnd.schedule.start_hour", 22);
            sched.start_minute = cfg.get<int>("dnd.schedule.start_minute", 0);
            sched.end_hour = cfg.get<int>("dnd.schedule.end_hour", 7);
            sched.end_minute = cfg.get<int>("dnd.schedule.end_minute", 0);
            engine_.set_dnd_schedule(sched);
        }

        // Load persisted state.
        engine_.load_history(kHistoryPath);
        engine_.load_rules(kRulesPath);

        // Set up display callback (writes to stdout for now; in production
        // this would render via the compositor overlay).
        engine_.set_display_callback([](const Notification& n) {
            SL_INFO("notify: displaying [{}/{}] {}: {}",
                    n.app_name,
                    (n.urgency == Urgency::Critical ? "CRITICAL" :
                     n.urgency == Urgency::Low ? "low" : "normal"),
                    n.summary, n.body);
        });

        // Bind the IPC socket.
        auto bind_result = ipc_server_.bind(kSocketPath);
        if (!bind_result.has_value()) {
            SL_WARN("notify: cannot bind IPC socket: {}", bind_result.error());
            // Non-fatal; D-Bus is the primary interface.
        }

        // Open D-Bus bridge (non-fatal if session bus is unavailable).
        auto dbus_result = dbus_bridge_.open();
        if (!dbus_result.has_value()) {
            SL_WARN("notify: D-Bus bridge not available: {}",
                    dbus_result.error().message());
        }

        SL_INFO("notify: daemon initialized");
        return Result<void, SLError>::ok();
    }

    Result<void, SLError> tick() override {
        // Accept IPC connections (non-blocking with short timeout).
        auto conn_result = ipc_server_.accept(100);
        if (conn_result.has_value()) {
            auto& conn = conn_result.value();
            timeval timeout{};
            timeout.tv_sec = 2;
            setsockopt(conn->fd(), SOL_SOCKET, SO_RCVTIMEO,
                       &timeout, sizeof(timeout));
            auto msg_result = conn->receive();
            if (msg_result.has_value()) {
                auto response = handle_ipc(msg_result.value());
                conn->send(response);
            } else {
                const auto& err = msg_result.error();
                if (err == "Failed to receive message length") {
                    SL_DEBUG("notify: empty IPC connection closed before request frame");
                } else {
                    SL_WARN("notify: dropped incomplete IPC request: {}", err);
                }
            }
        }

        // Process pending D-Bus messages (non-blocking).
        dbus_bridge_.process();

        // Small sleep to avoid busy-waiting.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        return Result<void, SLError>::ok();
    }

    void shutdown() override {
        SL_INFO("notify: shutting down");
        dbus_bridge_.close();
        engine_.save_history(kHistoryPath);
        engine_.save_rules(kRulesPath);
    }

private:
    /// Handle an IPC message from straylight-notify-cli.
    /// Protocol: newline-delimited command strings.
    /// Returns a response string.
    std::string handle_ipc(const std::string& msg) {
        // Parse command.
        std::istringstream iss(msg);
        std::string cmd;
        iss >> cmd;

        if (cmd == "SEND") {
            // SEND <urgency> <app> <summary> <body>
            std::string urgency_str, app, summary, body;
            iss >> urgency_str >> app;
            std::getline(iss, summary, '\t');
            std::getline(iss, body);

            // Trim leading space from summary.
            if (!summary.empty() && summary[0] == ' ') summary = summary.substr(1);

            auto r = engine_.notify(app, summary, body,
                                    parse_urgency(urgency_str), {}, "", 5000);
            if (r.has_value()) {
                return "OK " + std::to_string(r.value());
            }
            return "ERR " + r.error().message();

        } else if (cmd == "HISTORY") {
            std::string app_filter;
            iss >> app_filter;
            auto hist = engine_.history(app_filter);

            std::ostringstream oss;
            for (const auto& n : hist) {
                auto epoch = std::chrono::duration_cast<std::chrono::seconds>(
                    n.timestamp.time_since_epoch()).count();
                oss << n.id << "\t" << n.app_name << "\t" << n.summary
                    << "\t" << n.body << "\t" << epoch << "\t"
                    << (n.dismissed ? "dismissed" : "active") << "\n";
            }
            return oss.str().empty() ? "EMPTY\n" : oss.str();

        } else if (cmd == "CLEAR") {
            engine_.clear_history();
            return "OK";

        } else if (cmd == "DND") {
            std::string subcmd;
            iss >> subcmd;
            if (subcmd == "on") {
                engine_.set_dnd(true);
                return "OK DND enabled";
            } else if (subcmd == "off") {
                engine_.set_dnd(false);
                return "OK DND disabled";
            } else if (subcmd == "status") {
                return engine_.dnd_enabled() ? "DND on" : "DND off";
            } else if (subcmd == "schedule") {
                int sh, sm, eh, em;
                if (iss >> sh >> sm >> eh >> em) {
                    DndSchedule sched{true, sh, sm, eh, em};
                    engine_.set_dnd_schedule(sched);
                    return "OK schedule set";
                }
                auto sched = engine_.get_dnd_schedule();
                std::ostringstream oss;
                oss << "Schedule: " << (sched.enabled ? "enabled" : "disabled")
                    << " " << sched.start_hour << ":" << sched.start_minute
                    << " - " << sched.end_hour << ":" << sched.end_minute;
                return oss.str();
            }
            return "ERR unknown DND command";

        } else if (cmd == "RULE") {
            std::string subcmd;
            iss >> subcmd;
            if (subcmd == "add") {
                NotificationRule rule;
                std::string action_str;
                iss >> rule.app_pattern >> action_str;
                if (action_str == "suppress") rule.action = RuleAction::Suppress;
                else if (action_str == "modify") rule.action = RuleAction::Modify;
                else if (action_str == "redirect") rule.action = RuleAction::Redirect;
                auto r = engine_.add_rule(rule);
                if (r.has_value()) return "OK " + std::to_string(r.value());
                return "ERR " + r.error().message();
            } else if (subcmd == "list") {
                auto rules = engine_.list_rules();
                std::ostringstream oss;
                for (const auto& r : rules) {
                    static const char* action_names[] = {"show","suppress","modify","redirect"};
                    oss << r.id << "\t" << r.app_pattern << "\t"
                        << action_names[static_cast<int>(r.action)] << "\t"
                        << (r.enabled ? "enabled" : "disabled") << "\n";
                }
                return oss.str().empty() ? "EMPTY\n" : oss.str();
            } else if (subcmd == "remove") {
                uint32_t id;
                iss >> id;
                auto r = engine_.remove_rule(id);
                return r.has_value() ? "OK" : "ERR " + r.error().message();
            }
            return "ERR unknown RULE command";
        }

        return "ERR unknown command: " + cmd;
    }

    NotificationEngine engine_;
    DBusBridge         dbus_bridge_{engine_};
    IpcServer          ipc_server_;
};

} // namespace straylight

int main(int /*argc*/, char* /*argv*/[]) {
    straylight::Log::init("straylight-notify");

    auto cfg_result = straylight::Config::load("/etc/straylight/notify.conf");
    if (!cfg_result.has_value()) {
        SL_WARN("notify: no config found at /etc/straylight/notify.conf, using empty defaults");
        // Construct an empty JSON-backed config directly (no file needed).
        straylight::NotifyDaemon daemon;
        return daemon.run(straylight::Config::make_empty());
    }

    straylight::NotifyDaemon daemon;
    return daemon.run(cfg_result.value());
}
