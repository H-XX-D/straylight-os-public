// services/watchdog/main.cpp
// straylight-watchdog daemon — monitors critical services, auto-restarts on crash.
#include "watchdog_engine.h"

#include <straylight/config.h>
#include <straylight/daemon.h>
#include <straylight/ipc.h>
#include <straylight/log.h>

#include <chrono>
#include <thread>

#include <nlohmann/json.hpp>

namespace straylight {

class WatchdogDaemon : public DaemonBase {
public:
    Result<void, SLError> init(const Config& cfg) override {
        SL_INFO("watchdog: initializing daemon");

        tick_interval_s_ = cfg.get<int>("tick_interval_seconds", 10);
        socket_path_ = cfg.get<std::string>(
            "ipc.socket_path", "/run/straylight/watchdog.sock");

        // Load watched services from config directory
        std::string watch_dir = cfg.get<std::string>(
            "watch_directory", "/etc/straylight/watchdog.d");
        auto load_res = engine_.load_config(watch_dir);
        if (!load_res.has_value()) {
            SL_WARN("watchdog: could not load watch configs: {}", load_res.error().message());
        }

        // Load inline services from config
        auto raw = cfg.raw();
        if (raw.contains("services") && raw["services"].is_array()) {
            for (const auto& svc_json : raw["services"]) {
                WatchedService svc;
                svc.name = svc_json.value("name", "");
                svc.unit_name = svc_json.value("unit", svc.name + ".service");
                svc.max_retries = svc_json.value("max_retries", 3);
                svc.backoff_base_seconds = svc_json.value("backoff_seconds", 5);
                svc.health_check_interval_seconds = svc_json.value("check_interval_seconds", 30);

                std::string ct = svc_json.value("check_type", "proc");
                if (ct == "http") svc.check_type = HealthCheckType::HttpGet;
                else if (ct == "socket") svc.check_type = HealthCheckType::UnixSocket;
                else if (ct == "file") svc.check_type = HealthCheckType::FileTouch;
                else svc.check_type = HealthCheckType::ProcStat;

                svc.check_target = svc_json.value("check_target", "");

                if (!svc.name.empty()) {
                    auto res = engine_.watch(svc);
                    if (!res.has_value()) {
                        SL_WARN("watchdog: failed to add '{}': {}",
                                svc.name, res.error().message());
                    }
                }
            }
        }

        // Start IPC server
        auto bind_res = ipc_server_.bind(socket_path_);
        if (!bind_res.has_value()) {
            SL_WARN("watchdog: IPC bind failed: {}", bind_res.error());
        }

        auto services = engine_.list_services();
        SL_INFO("watchdog: daemon initialized, watching {} services (tick={}s)",
                services.size(), tick_interval_s_);
        return Result<void, SLError>::ok();
    }

    Result<void, SLError> tick() override {
        // Check all services
        engine_.check_all();

        // Handle IPC requests (non-blocking)
        handle_ipc();

        std::this_thread::sleep_for(std::chrono::seconds(tick_interval_s_));
        return Result<void, SLError>::ok();
    }

    void shutdown() override {
        SL_INFO("watchdog: shutting down");
        SL_INFO("watchdog: shutdown complete");
    }

private:
    void handle_ipc() {
        auto conn_res = ipc_server_.accept(100); // 100ms timeout
        if (!conn_res.has_value()) return;

        auto& conn = conn_res.value();
        auto msg_res = conn->receive();
        if (!msg_res.has_value()) return;

        try {
            auto req = nlohmann::json::parse(msg_res.value());
            std::string cmd = req.value("cmd", "");
            nlohmann::json response;

            if (cmd == "list") {
                auto services = engine_.list_services();
                nlohmann::json arr = nlohmann::json::array();
                for (const auto& name : services) {
                    auto st = engine_.get_state(name);
                    if (st.has_value()) {
                        nlohmann::json sj;
                        sj["name"] = st.value().name;
                        sj["pid"] = st.value().pid;
                        sj["running"] = st.value().running;
                        sj["failures"] = st.value().consecutive_failures;
                        sj["restarts"] = st.value().total_restarts;
                        arr.push_back(sj);
                    }
                }
                response["status"] = "ok";
                response["services"] = arr;

            } else if (cmd == "status") {
                std::string name = req["params"].value("name", "");
                auto st = engine_.get_state(name);
                if (st.has_value()) {
                    response["status"] = "ok";
                    response["name"] = st.value().name;
                    response["pid"] = st.value().pid;
                    response["running"] = st.value().running;
                    response["failures"] = st.value().consecutive_failures;
                    response["restarts"] = st.value().total_restarts;
                    response["last_error"] = st.value().last_error;
                } else {
                    response["status"] = "error";
                    response["message"] = st.error().message();
                }

            } else if (cmd == "watch") {
                WatchedService svc;
                auto& p = req["params"];
                svc.name = p.value("name", "");
                svc.unit_name = p.value("unit", svc.name + ".service");
                svc.max_retries = p.value("max_retries", 3);
                auto res = engine_.watch(svc);
                response["status"] = res.has_value() ? "ok" : "error";
                if (!res.has_value()) response["message"] = res.error().message();

            } else if (cmd == "unwatch") {
                std::string name = req["params"].value("name", "");
                auto res = engine_.unwatch(name);
                response["status"] = res.has_value() ? "ok" : "error";
                if (!res.has_value()) response["message"] = res.error().message();

            } else if (cmd == "history") {
                std::string name = req["params"].value("name", "");
                auto hist = engine_.get_history(name);
                if (hist.has_value()) {
                    response["status"] = "ok";
                    response["history"] = hist.value();
                } else {
                    response["status"] = "error";
                    response["message"] = hist.error().message();
                }

            } else {
                response["status"] = "error";
                response["message"] = "Unknown command: " + cmd;
            }

            conn->send(response.dump());
        } catch (const std::exception& e) {
            nlohmann::json err;
            err["status"] = "error";
            err["message"] = e.what();
            conn->send(err.dump());
        }
    }

    WatchdogEngine engine_;
    IpcServer ipc_server_;
    std::string socket_path_;
    int tick_interval_s_ = 10;
};

} // namespace straylight

int main(int /*argc*/, char* /*argv*/[]) {
    straylight::Log::init("straylight-watchdog");

    auto cfg_result = straylight::Config::load("/etc/straylight/watchdog.conf");
    if (!cfg_result.has_value()) {
        SL_ERROR("watchdog: failed to load config: {}", cfg_result.error());
        return 1;
    }

    straylight::WatchdogDaemon daemon;
    return daemon.run(cfg_result.value());
}
