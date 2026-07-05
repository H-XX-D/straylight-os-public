// services/secrets/main.cpp
// straylight-secrets daemon — system-wide secrets manager.
#include "secrets_engine.h"

#include <straylight/config.h>
#include <straylight/daemon.h>
#include <straylight/ipc.h>
#include <straylight/log.h>

#include <algorithm>
#include <chrono>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <thread>

#include <nlohmann/json.hpp>

namespace straylight {

class SecretsDaemon : public DaemonBase {
public:
    Result<void, SLError> init(const Config& cfg) override {
        SL_INFO("secrets: initializing daemon");

        tick_interval_s_ = std::max(1, cfg.get<int>("tick_interval_seconds", 60));
        socket_path_ = cfg.get<std::string>(
            "ipc.socket_path", "/run/straylight/secrets.sock");
        std::string store_path = cfg.get<std::string>(
            "store_path", "/var/lib/straylight/secrets/store.json");
        std::string key_path = cfg.get<std::string>(
            "master_key_path", "/var/lib/straylight/secrets/master.key");

        auto init_res = engine_.init(store_path, key_path);
        if (!init_res.has_value()) {
            return init_res;
        }

        auto bind_res = ipc_server_.bind(socket_path_);
        if (!bind_res.has_value()) {
            return Result<void, SLError>::error(
                SLError{SLErrorCode::Internal, "IPC bind failed: " + bind_res.error()});
        }

        SL_INFO("secrets: daemon initialized");
        last_rotation_check_ = std::chrono::steady_clock::now();
        return Result<void, SLError>::ok();
    }

    Result<void, SLError> tick() override {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_rotation_check_);
        if (elapsed.count() >= tick_interval_s_) {
            engine_.check_rotations();
            last_rotation_check_ = now;
        }

        handle_ipc();

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return Result<void, SLError>::ok();
    }

    void shutdown() override {
        SL_INFO("secrets: shutting down");
    }

private:
    void handle_ipc() {
        auto conn_res = ipc_server_.accept(100);
        if (!conn_res.has_value()) return;

        auto& conn = conn_res.value();
        timeval tv{};
        tv.tv_sec = 2;
        setsockopt(conn->fd(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        auto msg_res = conn->receive();
        if (!msg_res.has_value()) return;

        try {
            auto req = nlohmann::json::parse(msg_res.value());
            std::string cmd = req.value("cmd", "");
            uint32_t caller_uid = req.value("uid", uint32_t(0));
            nlohmann::json response;

            if (cmd == "set") {
                std::string key = req["params"].value("key", "");
                std::string value = req["params"].value("value", "");
                auto res = engine_.set(key, value, caller_uid);
                response["status"] = res.has_value() ? "ok" : "error";
                if (!res.has_value()) response["message"] = res.error().message();

            } else if (cmd == "get") {
                std::string key = req["params"].value("key", "");
                auto res = engine_.get(key, caller_uid);
                if (res.has_value()) {
                    response["status"] = "ok";
                    response["value"] = res.value();
                } else {
                    response["status"] = "error";
                    response["message"] = res.error().message();
                }

            } else if (cmd == "delete") {
                std::string key = req["params"].value("key", "");
                auto res = engine_.remove(key, caller_uid);
                response["status"] = res.has_value() ? "ok" : "error";
                if (!res.has_value()) response["message"] = res.error().message();

            } else if (cmd == "list") {
                auto keys = engine_.list(caller_uid);
                response["status"] = "ok";
                response["keys"] = keys;

            } else if (cmd == "rotate") {
                std::string key = req["params"].value("key", "");
                auto res = engine_.rotate(key, caller_uid);
                response["status"] = res.has_value() ? "ok" : "error";
                if (!res.has_value()) response["message"] = res.error().message();

            } else if (cmd == "acl-add") {
                std::string key = req["params"].value("key", "");
                uint32_t target = req["params"].value("uid", uint32_t(0));
                auto res = engine_.acl_add(key, target, caller_uid);
                response["status"] = res.has_value() ? "ok" : "error";
                if (!res.has_value()) response["message"] = res.error().message();

            } else if (cmd == "acl-remove") {
                std::string key = req["params"].value("key", "");
                uint32_t target = req["params"].value("uid", uint32_t(0));
                auto res = engine_.acl_remove(key, target, caller_uid);
                response["status"] = res.has_value() ? "ok" : "error";
                if (!res.has_value()) response["message"] = res.error().message();

            } else if (cmd == "env") {
                auto env = engine_.get_env_for(caller_uid);
                response["status"] = "ok";
                response["env"] = nlohmann::json(env);

            } else if (cmd == "audit") {
                int n = 100;
                if (req.contains("params")) {
                    n = req["params"].value("last_n", 100);
                }
                auto log = engine_.get_audit_log(n);
                nlohmann::json arr = nlohmann::json::array();
                for (const auto& e : log) {
                    nlohmann::json ej;
                    ej["action"] = e.action;
                    ej["key"] = e.key;
                    ej["uid"] = e.uid;
                    ej["allowed"] = e.allowed;
                    ej["detail"] = e.detail;
                    arr.push_back(ej);
                }
                response["status"] = "ok";
                response["entries"] = arr;

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

    SecretsEngine engine_;
    IpcServer ipc_server_;
    std::string socket_path_;
    int tick_interval_s_ = 60;
    std::chrono::steady_clock::time_point last_rotation_check_{};
};

} // namespace straylight

int main(int /*argc*/, char* /*argv*/[]) {
    straylight::Log::init("straylight-secrets");

    auto cfg_result = straylight::Config::load("/etc/straylight/secrets.conf");
    if (!cfg_result.has_value()) {
        SL_ERROR("secrets: failed to load config: {}", cfg_result.error());
        return 1;
    }

    straylight::SecretsDaemon daemon;
    return daemon.run(cfg_result.value());
}
