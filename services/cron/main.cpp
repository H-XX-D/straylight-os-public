// services/cron/main.cpp
// straylight-cron daemon — Smart task scheduler with dependency awareness.
#include "scheduler.h"

#include <straylight/config.h>
#include <straylight/daemon.h>
#include <straylight/log.h>

#include <nlohmann/json.hpp>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <grp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace straylight {

/// Cron daemon — wraps CronScheduler in the DaemonBase lifecycle.
class CronDaemon : public DaemonBase {
public:
    Result<void, SLError> init(const Config& cfg) override {
        SL_INFO("cron: initializing daemon");

        config_path_ = cfg.get<std::string>(
            "tasks_file", "/etc/straylight/cron-tasks.json");
        tick_interval_s_ = cfg.get<int>("tick_interval_seconds", 10);
        auto_save_ = cfg.get<bool>("auto_save", true);
        socket_path_ = cfg.get<std::string>(
            "ipc.socket_path", "/run/straylight/cron.sock");

        auto lr = scheduler_.load_tasks(config_path_);
        if (lr.has_value()) {
            SL_INFO("cron: loaded {} task(s) from {}", lr.value(), config_path_);
        } else {
            SL_WARN("cron: no tasks loaded: {}", lr.error());
        }

        // Catch up any missed runs from downtime
        int caught = scheduler_.catch_up_missed();
        if (caught > 0) {
            SL_INFO("cron: caught up {} missed task(s)", caught);
        }

        auto ipc = start_ipc_server();
        if (!ipc.has_value()) {
            return Result<void, SLError>::error(
                SLError{SLErrorCode::IpcFailed, ipc.error()});
        }

        SL_INFO("cron: daemon initialized (tick={}s)", tick_interval_s_);
        return Result<void, SLError>::ok();
    }

    Result<void, SLError> tick() override {
        auto r = scheduler_.tick();
        if (r.has_value() && r.value() > 0) {
            SL_DEBUG("cron: tick executed {} task(s)", r.value());

            if (auto_save_) {
                auto sr = scheduler_.save_tasks(config_path_);
                if (!sr.has_value()) {
                    SL_WARN("cron: auto-save failed: {}", sr.error());
                }
            }
        }

        service_ipc_for(std::chrono::seconds(tick_interval_s_));
        return Result<void, SLError>::ok();
    }

    void shutdown() override {
        SL_INFO("cron: shutting down");
        auto sr = scheduler_.save_tasks(config_path_);
        if (!sr.has_value()) {
            SL_WARN("cron: save on shutdown failed: {}", sr.error());
        }
        if (server_fd_ >= 0) {
            ::close(server_fd_);
            server_fd_ = -1;
        }
        if (!socket_path_.empty()) {
            ::unlink(socket_path_.c_str());
        }
        SL_INFO("cron: shutdown complete");
    }

private:
    Result<void, std::string> start_ipc_server() {
        namespace fs = std::filesystem;

        std::error_code ec;
        fs::create_directories(fs::path(socket_path_).parent_path(), ec);
        if (ec) {
            return Result<void, std::string>::error(
                "cannot create socket directory: " + ec.message());
        }

        if (server_fd_ >= 0) {
            ::close(server_fd_);
            server_fd_ = -1;
        }
        ::unlink(socket_path_.c_str());

        server_fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (server_fd_ < 0) {
            return Result<void, std::string>::error(
                std::string("socket() failed: ") + std::strerror(errno));
        }

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

        if (::bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            std::string err = std::string("bind() failed: ") + std::strerror(errno);
            ::close(server_fd_);
            server_fd_ = -1;
            return Result<void, std::string>::error(err);
        }

        if (auto* group = ::getgrnam("straylight")) {
            ::chown(socket_path_.c_str(), -1, group->gr_gid);
        }
        ::chmod(socket_path_.c_str(), 0660);

        if (::listen(server_fd_, 16) < 0) {
            std::string err = std::string("listen() failed: ") + std::strerror(errno);
            ::close(server_fd_);
            server_fd_ = -1;
            ::unlink(socket_path_.c_str());
            return Result<void, std::string>::error(err);
        }

        SL_INFO("cron: IPC listening on {}", socket_path_);
        return Result<void, std::string>::ok();
    }

    static nlohmann::json task_to_json(const Task& task) {
        return nlohmann::json::parse(TaskSerializer::to_json(task));
    }

    static nlohmann::json run_to_json(const TaskRun& run) {
        return nlohmann::json::parse(TaskSerializer::run_to_json(run));
    }

    nlohmann::json error_response(const nlohmann::json& request,
                                  const std::string& message,
                                  int code = -32000) const {
        nlohmann::json response;
        response["jsonrpc"] = "2.0";
        response["id"] = request.value("id", 0);
        response["error"]["code"] = code;
        response["error"]["message"] = message;
        return response;
    }

    nlohmann::json ok_response(const nlohmann::json& request,
                               const nlohmann::json& result) const {
        nlohmann::json response;
        response["jsonrpc"] = "2.0";
        response["id"] = request.value("id", 0);
        response["result"] = result;
        return response;
    }

    nlohmann::json dispatch(const nlohmann::json& request) {
        std::string method = request.value("method", request.value("cmd", ""));
        nlohmann::json params = request.value("params", nlohmann::json::object());

        try {
            if (method == "list") {
                nlohmann::json tasks = nlohmann::json::array();
                for (const auto& task : scheduler_.list_tasks()) {
                    tasks.push_back(task_to_json(task));
                }
                return ok_response(request, tasks);
            }

            if (method == "status") {
                if (params.contains("name")) {
                    auto task = scheduler_.get_task(params.value("name", ""));
                    if (!task.has_value()) return error_response(request, task.error(), -32602);
                    return ok_response(request, task_to_json(task.value()));
                }

                auto tasks = scheduler_.list_tasks();
                nlohmann::json result;
                result["task_count"] = tasks.size();
                result["enabled_count"] = 0;
                result["running_count"] = 0;
                result["socket_path"] = socket_path_;
                result["tasks_file"] = config_path_;
                for (const auto& task : tasks) {
                    if (task.enabled) result["enabled_count"] = result["enabled_count"].get<int>() + 1;
                    if (task.running) result["running_count"] = result["running_count"].get<int>() + 1;
                }
                return ok_response(request, result);
            }

            if (method == "add") {
                Task task;
                task.name = params.value("name", "");
                task.command = params.value("command", "");
                task.schedule.spec = params.value("schedule", "");
                task.schedule.missed_policy = params.value("missed_policy", "skip");
                task.max_retries = params.value("max_retries", 0);
                if (params.contains("depends_on") && params["depends_on"].is_array()) {
                    for (const auto& dep : params["depends_on"]) {
                        if (dep.is_string()) task.depends_on.push_back(dep.get<std::string>());
                    }
                }
                task.resources.max_cpu_percent = params.value("max_cpu_percent", 80.0);
                task.resources.min_free_memory_mb =
                    params.value("min_free_memory_mb", uint64_t{1024});

                if (task.schedule.spec.empty()) {
                    return error_response(request, "schedule is required", -32602);
                }

                auto res = scheduler_.add_task(std::move(task));
                if (!res.has_value()) return error_response(request, res.error(), -32602);
                auto save = scheduler_.save_tasks(config_path_);
                if (!save.has_value()) return error_response(request, save.error(), -32001);
                return ok_response(request, {{"status", "ok"}});
            }

            if (method == "remove") {
                auto res = scheduler_.remove_task(params.value("name", ""));
                if (!res.has_value()) return error_response(request, res.error(), -32602);
                auto save = scheduler_.save_tasks(config_path_);
                if (!save.has_value()) return error_response(request, save.error(), -32001);
                return ok_response(request, {{"status", "ok"}});
            }

            if (method == "enable" || method == "disable") {
                auto res = scheduler_.set_enabled(params.value("name", ""), method == "enable");
                if (!res.has_value()) return error_response(request, res.error(), -32602);
                auto save = scheduler_.save_tasks(config_path_);
                if (!save.has_value()) return error_response(request, save.error(), -32001);
                return ok_response(request, {{"status", "ok"}});
            }

            if (method == "run") {
                auto res = scheduler_.run_now(params.value("name", ""));
                if (!res.has_value()) return error_response(request, res.error(), -32602);
                auto save = scheduler_.save_tasks(config_path_);
                if (!save.has_value()) return error_response(request, save.error(), -32001);
                return ok_response(request, run_to_json(res.value()));
            }

            if (method == "history") {
                nlohmann::json runs = nlohmann::json::array();
                for (const auto& run : scheduler_.get_history(
                         params.value("name", ""), params.value("limit", 20))) {
                    runs.push_back(run_to_json(run));
                }
                return ok_response(request, runs);
            }
        } catch (const std::exception& e) {
            return error_response(request, e.what());
        }

        return error_response(request, "Unknown method: " + method, -32601);
    }

    static bool send_all(int fd, const std::string& payload) {
        size_t sent = 0;
        while (sent < payload.size()) {
            ssize_t n = ::send(fd, payload.data() + sent, payload.size() - sent, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    void handle_ipc_once(int timeout_ms) {
        if (server_fd_ < 0) return;

        pollfd pfd{server_fd_, POLLIN, 0};
        int pr = ::poll(&pfd, 1, timeout_ms);
        if (pr <= 0 || !(pfd.revents & POLLIN)) return;

        int client_fd = ::accept(server_fd_, nullptr, nullptr);
        if (client_fd < 0) return;

        std::string request_text;
        char buf[4096];
        while (request_text.find('\n') == std::string::npos &&
               request_text.size() < 1024 * 1024) {
            pollfd cpfd{client_fd, POLLIN, 0};
            int cr = ::poll(&cpfd, 1, 2000);
            if (cr <= 0 || !(cpfd.revents & POLLIN)) break;
            ssize_t n = ::read(client_fd, buf, sizeof(buf));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            request_text.append(buf, static_cast<size_t>(n));
        }

        nlohmann::json response;
        try {
            auto newline = request_text.find('\n');
            if (newline != std::string::npos) request_text.resize(newline);
            auto request = nlohmann::json::parse(request_text);
            response = dispatch(request);
        } catch (const std::exception& e) {
            nlohmann::json request = nlohmann::json::object();
            response = error_response(request,
                                      std::string("JSON parse error: ") + e.what(),
                                      -32700);
        }

        std::string payload = response.dump() + "\n";
        send_all(client_fd, payload);
        ::close(client_fd);
    }

    void service_ipc_for(std::chrono::seconds duration) {
        auto deadline = std::chrono::steady_clock::now() + duration;
        while (std::chrono::steady_clock::now() < deadline) {
            handle_ipc_once(250);
        }
    }

    CronScheduler scheduler_;
    std::string config_path_;
    std::string socket_path_;
    int server_fd_ = -1;
    int tick_interval_s_ = 10;
    bool auto_save_ = true;
};

} // namespace straylight

int main(int /*argc*/, char* /*argv*/[]) {
    straylight::Log::init("straylight-cron");

    auto cfg_result = straylight::Config::load("/etc/straylight/cron.conf");
    if (!cfg_result.has_value()) {
        SL_ERROR("cron: failed to load config: {}", cfg_result.error());
        return 1;
    }

    straylight::CronDaemon daemon;
    return daemon.run(cfg_result.value());
}
