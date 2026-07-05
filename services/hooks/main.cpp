// services/hooks/main.cpp
// straylight-hooks daemon — system event hook engine.
#include "hook_engine.h"

#include <straylight/config.h>
#include <straylight/daemon.h>
#include <straylight/ipc.h>
#include <straylight/log.h>

#include <chrono>
#include <thread>

#include <nlohmann/json.hpp>

#include <sys/socket.h>
#include <sys/time.h>

namespace straylight {

namespace {

void set_receive_timeout(IpcConnection& conn) {
    timeval tv{};
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    setsockopt(conn.fd(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

void send_response(IpcConnection& conn, const nlohmann::json& response) {
    auto send_res = conn.send(response.dump());
    if (!send_res.has_value()) {
        SL_WARN("hooks: failed to send response: {}", send_res.error());
    }
}

} // namespace

class HooksDaemon : public DaemonBase {
public:
    Result<void, SLError> init(const Config& cfg) override {
        SL_INFO("hooks: initializing daemon");

        tick_interval_s_ = cfg.get<int>("tick_interval_seconds", 5);
        socket_path_ = cfg.get<std::string>(
            "ipc.socket_path", "/run/straylight/hooks.sock");
        std::string hooks_dir = cfg.get<std::string>(
            "hooks_directory", "/etc/straylight/hooks.d");

        auto load_res = engine_.load_hooks(hooks_dir);
        if (!load_res.has_value()) {
            SL_WARN("hooks: load issue: {}", load_res.error().message());
        }

        auto bind_res = ipc_server_.bind(socket_path_);
        if (!bind_res.has_value()) {
            return Result<void, SLError>::error(
                SLError{SLErrorCode::IpcFailed,
                        "hooks: IPC bind failed: " + bind_res.error()});
        }

        // Fire boot event
        engine_.fire(SystemEvent::Boot);

        SL_INFO("hooks: daemon initialized");
        return Result<void, SLError>::ok();
    }

    Result<void, SLError> tick() override {
        handle_ipc();
        return Result<void, SLError>::ok();
    }

    void shutdown() override {
        SL_INFO("hooks: firing shutdown hooks");
        engine_.fire(SystemEvent::Shutdown);
        SL_INFO("hooks: shutdown complete");
    }

private:
    void handle_ipc() {
        auto conn_res = ipc_server_.accept(tick_interval_s_ * 1000);
        if (!conn_res.has_value()) return;

        auto& conn = conn_res.value();
        set_receive_timeout(*conn);
        auto msg_res = conn->receive();
        if (!msg_res.has_value()) {
            if (msg_res.error() == "Failed to receive message length") {
                SL_DEBUG("hooks: client closed before sending a request");
            } else {
                SL_WARN("hooks: failed to receive request: {}", msg_res.error());
            }
            return;
        }

        try {
            auto req = nlohmann::json::parse(msg_res.value());
            std::string cmd = req.value("cmd", "");
            nlohmann::json response;

            if (cmd == "list") {
                auto hooks = engine_.list_hooks();
                nlohmann::json arr = nlohmann::json::array();
                for (const auto& h : hooks) {
                    nlohmann::json hj;
                    hj["id"] = h.id;
                    hj["event"] = HookEngine::event_name(h.event);
                    hj["script"] = h.script_path;
                    hj["enabled"] = h.enabled;
                    hj["priority"] = h.priority;
                    hj["timeout"] = h.timeout_seconds;
                    arr.push_back(hj);
                }
                response["status"] = "ok";
                response["hooks"] = arr;

            } else if (cmd == "add") {
                const auto p = req.value("params", nlohmann::json::object());
                Hook hook;
                hook.id = p.value("id", HookEngine::generate_id());
                hook.script_path = p.value("script", "");
                hook.timeout_seconds = p.value("timeout_seconds", 30);
                hook.enabled = p.value("enabled", true);
                hook.priority = p.value("priority", 50);

                auto ev = HookEngine::parse_event(p.value("event", ""));
                if (!ev.has_value()) {
                    response["status"] = "error";
                    response["message"] = ev.error();
                } else {
                    hook.event = ev.value();
                    auto res = engine_.add_hook(hook);
                    response["status"] = res.has_value() ? "ok" : "error";
                    if (res.has_value()) response["id"] = hook.id;
                    if (!res.has_value()) response["message"] = res.error().message();
                }

            } else if (cmd == "remove") {
                const auto p = req.value("params", nlohmann::json::object());
                std::string id = p.value("id", "");
                auto res = engine_.remove_hook(id);
                response["status"] = res.has_value() ? "ok" : "error";
                if (!res.has_value()) response["message"] = res.error().message();

            } else if (cmd == "test") {
                const auto p = req.value("params", nlohmann::json::object());
                std::string event_str = p.value("event", "");
                auto ev = HookEngine::parse_event(event_str);
                if (!ev.has_value()) {
                    response["status"] = "error";
                    response["message"] = ev.error();
                } else {
                    auto results = engine_.test_fire(ev.value());
                    nlohmann::json arr = nlohmann::json::array();
                    for (const auto& r : results) {
                        nlohmann::json rj;
                        rj["hook_id"] = r.hook_id;
                        rj["exit_code"] = r.exit_code;
                        rj["timed_out"] = r.timed_out;
                        rj["duration_ms"] = r.duration.count();
                        if (!r.stdout_output.empty()) rj["stdout"] = r.stdout_output;
                        if (!r.stderr_output.empty()) rj["stderr"] = r.stderr_output;
                        arr.push_back(rj);
                    }
                    response["status"] = "ok";
                    response["results"] = arr;
                }

            } else if (cmd == "fire") {
                const auto p = req.value("params", nlohmann::json::object());
                std::string event_str = p.value("event", "");
                auto ev = HookEngine::parse_event(event_str);
                if (!ev.has_value()) {
                    response["status"] = "error";
                    response["message"] = ev.error();
                } else {
                    auto results = engine_.fire(ev.value());
                    response["status"] = "ok";
                    response["executed"] = static_cast<int>(results.size());
                }

            } else if (cmd == "history") {
                auto hist = engine_.get_history();
                nlohmann::json arr = nlohmann::json::array();
                for (const auto& h : hist) {
                    nlohmann::json hj;
                    hj["timestamp"] = h.timestamp;
                    hj["hook_id"] = h.hook_id;
                    hj["event"] = HookEngine::event_name(h.event);
                    hj["exit_code"] = h.exit_code;
                    hj["timed_out"] = h.timed_out;
                    hj["duration_ms"] = h.duration.count();
                    arr.push_back(hj);
                }
                response["status"] = "ok";
                response["history"] = arr;

            } else {
                response["status"] = "error";
                response["message"] = "Unknown command: " + cmd;
            }

            send_response(*conn, response);
        } catch (const std::exception& e) {
            nlohmann::json err;
            err["status"] = "error";
            err["message"] = e.what();
            send_response(*conn, err);
        }
    }

    HookEngine engine_;
    IpcServer ipc_server_;
    std::string socket_path_;
    int tick_interval_s_ = 5;
};

} // namespace straylight

int main(int /*argc*/, char* /*argv*/[]) {
    straylight::Log::init("straylight-hooks");

    auto cfg_result = straylight::Config::load("/etc/straylight/hooks.conf");
    if (!cfg_result.has_value()) {
        SL_ERROR("hooks: failed to load config: {}", cfg_result.error());
        return 1;
    }

    straylight::HooksDaemon daemon;
    return daemon.run(cfg_result.value());
}
