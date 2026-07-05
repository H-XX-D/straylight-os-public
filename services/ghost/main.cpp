// services/ghost/main.cpp
// straylight-ghost daemon — Transparent process migration between machines.

#include "migration_engine.h"
#include "page_server.h"
#include "process_restore.h"

#include <straylight/config.h>
#include <straylight/daemon.h>
#include <straylight/ipc.h>
#include <straylight/log.h>

#include <nlohmann/json.hpp>

#include <memory>
#include <string>

namespace straylight {

class GhostDaemon : public DaemonBase {
public:
    Result<void, SLError> init(const Config& cfg) override {
        SL_INFO("ghost: initializing daemon");

        socket_path_ = cfg.get<std::string>(
            "ipc.socket_path", "/run/straylight/ghost.sock");
        tick_interval_ms_ = cfg.get<int>("tick_interval_ms", 500);
        default_port_ = cfg.get<int>("page_server.port", 9730);
        compress_level_ = cfg.get<int>("compression.level", 3);

        engine_ = std::make_unique<MigrationEngine>();
        page_server_ = std::make_unique<PageServer>();
        engine_->set_page_server(page_server_.get());

        auto bind_result = server_.bind(socket_path_);
        if (!bind_result.has_value()) {
            return Result<void, SLError>::error(
                SLError{SLErrorCode::IpcFailed,
                        "Failed to bind socket: " + bind_result.error()});
        }

        SL_INFO("ghost: listening on {}", socket_path_);
        SL_INFO("ghost: daemon initialized (port={}, zstd_level={})",
                default_port_, compress_level_);

        return Result<void, SLError>::ok();
    }

    Result<void, SLError> tick() override {
        auto conn_result = server_.accept(tick_interval_ms_);
        if (conn_result.has_value()) {
            handle_connection(std::move(conn_result.value()));
        }
        return Result<void, SLError>::ok();
    }

    void shutdown() override {
        SL_INFO("ghost: shutting down");
        page_server_->stop();
        engine_.reset();
        SL_INFO("ghost: shutdown complete");
    }

private:
    void handle_connection(std::unique_ptr<IpcConnection> conn) {
        auto msg_result = conn->receive();
        if (!msg_result.has_value()) {
            SL_WARN("ghost: failed to receive: {}", msg_result.error());
            return;
        }

        nlohmann::json request;
        try {
            request = nlohmann::json::parse(msg_result.value());
        } catch (const nlohmann::json::parse_error& e) {
            nlohmann::json err;
            err["error"] = std::string("JSON parse error: ") + e.what();
            conn->send(err.dump() + "\n");
            return;
        }

        nlohmann::json response = dispatch(request);
        conn->send(response.dump() + "\n");
    }

    nlohmann::json dispatch(const nlohmann::json& request) {
        std::string method = request.value("method", "");
        nlohmann::json params = request.value("params", nlohmann::json::object());
        nlohmann::json response;
        response["jsonrpc"] = "2.0";
        response["id"] = request.value("id", 0);

        if (method == "migrate") {
            return handle_migrate(params, response, false);
        } else if (method == "lazy_migrate") {
            return handle_migrate(params, response, true);
        } else if (method == "status") {
            return handle_status(params, response);
        } else if (method == "list") {
            return handle_list(response);
        } else if (method == "cancel") {
            return handle_cancel(params, response);
        } else {
            response["error"] = "Unknown method: " + method;
        }
        return response;
    }

    nlohmann::json handle_migrate(const nlohmann::json& params,
                                    nlohmann::json response, bool lazy) {
        pid_t pid = params.value("pid", 0);
        std::string target = params.value("target_host", "");

        if (pid <= 0) {
            // Try resolving by name
            std::string name = params.value("name", "");
            if (!name.empty()) {
                // Search /proc for matching process
                for (int p = 1; p < 65536; ++p) {
                    std::ifstream comm("/proc/" + std::to_string(p) + "/comm");
                    if (comm.is_open()) {
                        std::string proc_name;
                        std::getline(comm, proc_name);
                        if (proc_name == name) {
                            pid = p;
                            break;
                        }
                    }
                }
            }
        }

        if (pid <= 0 || target.empty()) {
            response["error"]["code"] = static_cast<int>(SLErrorCode::InvalidArgument);
            response["error"]["message"] = "pid and target_host are required";
            return response;
        }

        MigrationOptions opts;
        opts.lazy = lazy;
        opts.port = params.value("port", default_port_);
        opts.compress_level = params.value("compress_level", compress_level_);
        opts.transfer_fds = params.value("transfer_fds", true);
        opts.transfer_network = params.value("transfer_network", false);

        Result<uint64_t, SLError> result = lazy
            ? engine_->lazy_migrate(pid, target, opts)
            : engine_->migrate(pid, target, opts);

        if (result.has_value()) {
            response["result"]["migration_id"] = result.value();
            response["result"]["pid"] = pid;
            response["result"]["target"] = target;
            response["result"]["mode"] = lazy ? "lazy" : "full";
            response["result"]["status"] = "started";
        } else {
            response["error"]["code"] = static_cast<int>(result.error().code());
            response["error"]["message"] = result.error().message();
        }
        return response;
    }

    nlohmann::json handle_status(const nlohmann::json& params, nlohmann::json response) {
        uint64_t id = params.value("migration_id", uint64_t{0});

        if (id == 0) {
            // Show all active migrations
            auto migrations = engine_->list_migrations();
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& m : migrations) {
                if (m.state != MigrationState::Complete &&
                    m.state != MigrationState::Failed &&
                    m.state != MigrationState::Cancelled) {
                    nlohmann::json entry;
                    entry["migration_id"] = m.migration_id;
                    entry["pid"] = m.source_pid;
                    entry["target"] = m.target_host;
                    entry["state"] = migration_state_name(m.state);
                    entry["pages_transferred"] = m.pages_transferred;
                    entry["pages_remaining"] = m.pages_remaining;
                    entry["bandwidth_mbps"] = m.bandwidth_mbps;
                    entry["elapsed_seconds"] = m.elapsed_seconds;
                    entry["eta_seconds"] = m.eta_seconds;
                    arr.push_back(entry);
                }
            }
            response["result"] = arr;
        } else {
            auto result = engine_->get_status(id);
            if (result.has_value()) {
                const auto& m = result.value();
                response["result"]["migration_id"] = m.migration_id;
                response["result"]["pid"] = m.source_pid;
                response["result"]["target"] = m.target_host;
                response["result"]["state"] = migration_state_name(m.state);
                response["result"]["total_pages"] = m.total_pages;
                response["result"]["pages_transferred"] = m.pages_transferred;
                response["result"]["pages_remaining"] = m.pages_remaining;
                response["result"]["hot_pages"] = m.hot_pages;
                response["result"]["bandwidth_mbps"] = m.bandwidth_mbps;
                response["result"]["elapsed_seconds"] = m.elapsed_seconds;
                response["result"]["eta_seconds"] = m.eta_seconds;
                if (!m.error_message.empty()) {
                    response["result"]["error"] = m.error_message;
                }
            } else {
                response["error"]["code"] = static_cast<int>(result.error().code());
                response["error"]["message"] = result.error().message();
            }
        }
        return response;
    }

    nlohmann::json handle_list(nlohmann::json response) {
        auto migrations = engine_->list_migrations();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& m : migrations) {
            nlohmann::json entry;
            entry["migration_id"] = m.migration_id;
            entry["pid"] = m.source_pid;
            entry["target"] = m.target_host;
            entry["state"] = migration_state_name(m.state);
            entry["pages_transferred"] = m.pages_transferred;
            entry["total_pages"] = m.total_pages;
            if (!m.error_message.empty()) {
                entry["error"] = m.error_message;
            }
            arr.push_back(entry);
        }
        response["result"] = arr;
        return response;
    }

    nlohmann::json handle_cancel(const nlohmann::json& params, nlohmann::json response) {
        uint64_t id = params.value("migration_id", uint64_t{0});
        auto result = engine_->cancel(id);
        if (result.has_value()) {
            response["result"]["status"] = "cancelled";
            response["result"]["migration_id"] = id;
        } else {
            response["error"]["code"] = static_cast<int>(result.error().code());
            response["error"]["message"] = result.error().message();
        }
        return response;
    }

    IpcServer server_;
    std::unique_ptr<MigrationEngine> engine_;
    std::unique_ptr<PageServer> page_server_;

    std::string socket_path_;
    int tick_interval_ms_{500};
    int default_port_{9730};
    int compress_level_{3};
};

} // namespace straylight

int main(int /*argc*/, char* /*argv*/[]) {
    straylight::Log::init("straylight-ghost");

    auto cfg_result = straylight::Config::load("/etc/straylight/ghost.conf");
    if (!cfg_result.has_value()) {
        SL_WARN("ghost: no config file, using defaults");
        auto default_cfg = straylight::Config::load("/dev/null");
        if (!default_cfg.has_value()) {
            SL_ERROR("ghost: cannot initialize");
            return 1;
        }
        straylight::GhostDaemon daemon;
        return daemon.run(default_cfg.value());
    }

    straylight::GhostDaemon daemon;
    return daemon.run(cfg_result.value());
}
