// services/policy/main.cpp
// straylight-policy daemon — Declarative system roles.

#include "policy_engine.h"

#include <straylight/config.h>
#include <straylight/daemon.h>
#include <straylight/ipc.h>
#include <straylight/log.h>

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>

namespace straylight {

class PolicyDaemon : public DaemonBase {
public:
    Result<void, SLError> init(const Config& cfg) override {
        SL_INFO("policy: initializing daemon");

        socket_path_ = cfg.get<std::string>(
            "ipc.socket_path", "/run/straylight/policy.sock");
        policy_dir_ = cfg.get<std::string>(
            "policy_dir", "/etc/straylight/policies");
        tick_interval_ms_ = cfg.get<int>("tick_interval_ms", 1000);
        compliance_check_interval_s_ = cfg.get<int>("compliance_check_interval_s", 300);

        engine_ = std::make_unique<PolicyEngine>();
        auto init_result = engine_->init(policy_dir_);
        if (!init_result.has_value()) {
            return init_result;
        }

        auto bind_result = server_.bind(socket_path_);
        if (!bind_result.has_value()) {
            return Result<void, SLError>::error(
                SLError{SLErrorCode::IpcFailed,
                        "Failed to bind socket: " + bind_result.error()});
        }

        // Apply saved role on startup if configured
        if (cfg.has("auto_apply") && cfg.get<bool>("auto_apply", false)) {
            std::string current = engine_->get_current_role();
            if (!current.empty()) {
                SL_INFO("policy: auto-applying saved role '{}'", current);
                engine_->apply_role(current);
            }
        }

        SL_INFO("policy: listening on {}", socket_path_);
        SL_INFO("policy: daemon initialized (policy_dir={}, roles={})",
                policy_dir_, engine_->list_roles().size());

        return Result<void, SLError>::ok();
    }

    Result<void, SLError> tick() override {
        auto conn_result = server_.accept(tick_interval_ms_);
        if (conn_result.has_value()) {
            handle_connection(std::move(conn_result).value());
        }

        // Periodic compliance check
        tick_count_++;
        int compliance_ticks = (compliance_check_interval_s_ * 1000) / tick_interval_ms_;
        if (compliance_ticks > 0 && tick_count_ % compliance_ticks == 0) {
            auto current = engine_->get_current_role();
            if (!current.empty()) {
                auto report = engine_->get_compliance();
                if (report.has_value() && !report.value().compliant) {
                    SL_WARN("policy: compliance deviation detected for role '{}' "
                            "({} issues)", current, report.value().failed_checks);
                }
            }
        }

        return Result<void, SLError>::ok();
    }

    void shutdown() override {
        SL_INFO("policy: shutting down");
        SL_INFO("policy: shutdown complete");
    }

private:
    void handle_connection(std::unique_ptr<IpcConnection> conn) {
        timeval timeout{};
        timeout.tv_sec = 2;
        setsockopt(conn->fd(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        auto msg_result = conn->receive();
        if (!msg_result.has_value()) {
            SL_DEBUG("policy: empty or incomplete IPC request: {}", msg_result.error());
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

        if (method == "apply") return handle_apply(params, response);
        else if (method == "status") return handle_status(response);
        else if (method == "list") return handle_list(response);
        else if (method == "check") return handle_check(response);
        else if (method == "create") return handle_create(params, response);
        else if (method == "diff") return handle_diff(params, response);
        else if (method == "show") return handle_show(params, response);
        else response["error"] = "Unknown method: " + method;

        return response;
    }

    nlohmann::json handle_apply(const nlohmann::json& params, nlohmann::json response) {
        std::string role = params.value("role", "");
        if (role.empty()) {
            response["error"] = "role name required";
            return response;
        }

        auto result = engine_->apply_role(role);
        if (result.has_value()) {
            const auto& r = result.value();
            response["result"]["role"] = r.role_name;
            response["result"]["success"] = r.success;
            response["result"]["changes_made"] = r.changes_made;
            response["result"]["changes_failed"] = r.changes_failed;
            response["result"]["actions"] = r.actions;
            if (!r.errors.empty()) {
                response["result"]["errors"] = r.errors;
            }
        } else {
            response["error"]["code"] = static_cast<int>(result.error().code());
            response["error"]["message"] = result.error().message();
        }
        return response;
    }

    nlohmann::json handle_status(nlohmann::json response) {
        std::string current = engine_->get_current_role();
        response["result"]["current_role"] = current.empty() ? "none" : current;

        if (!current.empty()) {
            auto role_result = engine_->get_role(current);
            if (role_result.has_value()) {
                const auto& role = role_result.value();
                response["result"]["description"] = role.description;
                response["result"]["autotune_profile"] = role.autotune_profile;
                response["result"]["compositor_mode"] = role.compositor.mode;
                response["result"]["active_services"] = role.active_services;
                response["result"]["disabled_services"] = role.disabled_services;
            }

            auto compliance = engine_->get_compliance();
            if (compliance.has_value()) {
                const auto& cr = compliance.value();
                response["result"]["compliant"] = cr.compliant;
                response["result"]["compliance_ratio"] = cr.compliance_ratio();
                response["result"]["checks_passed"] = cr.passed_checks;
                response["result"]["checks_failed"] = cr.failed_checks;
            }
        }

        response["result"]["available_roles"] = engine_->list_roles();
        return response;
    }

    nlohmann::json handle_list(nlohmann::json response) {
        auto roles = engine_->list_roles();
        nlohmann::json arr = nlohmann::json::array();

        for (const auto& name : roles) {
            auto role_result = engine_->get_role(name);
            nlohmann::json entry;
            entry["name"] = name;
            if (role_result.has_value()) {
                entry["description"] = role_result.value().description;
                entry["autotune_profile"] = role_result.value().autotune_profile;
                entry["compositor_mode"] = role_result.value().compositor.mode;
            }
            entry["active"] = (name == engine_->get_current_role());
            arr.push_back(entry);
        }

        response["result"] = arr;
        return response;
    }

    nlohmann::json handle_check(nlohmann::json response) {
        auto result = engine_->get_compliance();
        if (result.has_value()) {
            const auto& r = result.value();
            response["result"]["role"] = r.role_name;
            response["result"]["compliant"] = r.compliant;
            response["result"]["total_checks"] = r.total_checks;
            response["result"]["passed"] = r.passed_checks;
            response["result"]["failed"] = r.failed_checks;
            response["result"]["compliance_ratio"] = r.compliance_ratio();

            nlohmann::json deviations = nlohmann::json::array();
            for (const auto& d : r.deviations) {
                nlohmann::json dj;
                dj["component"] = d.component;
                dj["expected"] = d.expected;
                dj["actual"] = d.actual;
                dj["severity"] = deviation_severity_name(d.severity);
                dj["description"] = d.description;
                deviations.push_back(dj);
            }
            response["result"]["deviations"] = deviations;
        } else {
            response["error"]["code"] = static_cast<int>(result.error().code());
            response["error"]["message"] = result.error().message();
        }
        return response;
    }

    nlohmann::json handle_create(const nlohmann::json& params, nlohmann::json response) {
        std::string name = params.value("name", "");
        std::string base = params.value("base", "");
        nlohmann::json overrides = params.value("overrides", nlohmann::json::object());

        if (name.empty()) {
            response["error"] = "name is required";
            return response;
        }

        auto result = engine_->create_role(name, base, overrides);
        if (result.has_value()) {
            response["result"]["status"] = "created";
            response["result"]["name"] = name;
            response["result"]["base"] = base;
        } else {
            response["error"]["code"] = static_cast<int>(result.error().code());
            response["error"]["message"] = result.error().message();
        }
        return response;
    }

    nlohmann::json handle_diff(const nlohmann::json& params, nlohmann::json response) {
        std::string a = params.value("role_a", "");
        std::string b = params.value("role_b", "");

        if (a.empty() || b.empty()) {
            response["error"] = "role_a and role_b are required";
            return response;
        }

        response["result"] = engine_->diff_roles(a, b);
        return response;
    }

    nlohmann::json handle_show(const nlohmann::json& params, nlohmann::json response) {
        std::string name = params.value("role", "");
        if (name.empty()) {
            response["error"] = "role name required";
            return response;
        }

        auto result = engine_->get_role(name);
        if (result.has_value()) {
            response["result"] = result.value().to_json();
        } else {
            response["error"]["code"] = static_cast<int>(result.error().code());
            response["error"]["message"] = result.error().message();
        }
        return response;
    }

    IpcServer server_;
    std::unique_ptr<PolicyEngine> engine_;

    std::string socket_path_;
    std::string policy_dir_;
    int tick_interval_ms_{1000};
    int compliance_check_interval_s_{300};
    int tick_count_{0};
};

} // namespace straylight

int main(int /*argc*/, char* /*argv*/[]) {
    straylight::Log::init("straylight-policy");

    auto cfg_result = straylight::Config::load("/etc/straylight/policy.conf");
    if (!cfg_result.has_value()) {
        SL_WARN("policy: no config file, using defaults");
        auto default_cfg = straylight::Config::load("/dev/null");
        if (!default_cfg.has_value()) {
            SL_ERROR("policy: cannot initialize");
            return 1;
        }
        straylight::PolicyDaemon daemon;
        return daemon.run(default_cfg.value());
    }

    straylight::PolicyDaemon daemon;
    return daemon.run(cfg_result.value());
}
