// services/weave/main.cpp
// straylight-weave — dynamic service composition daemon.
// Wire services together into zero-copy pipelines at runtime.

#include "pipeline_engine.h"
#include "pipeline_parser.h"
#include "node_registry.h"

#include <straylight/daemon.h>
#include <straylight/config.h>
#include <straylight/log.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>

namespace straylight {

/// IPC server for weave daemon.
class WeaveIpcServer {
public:
    WeaveIpcServer() = default;
    ~WeaveIpcServer() { stop(); }

    void set_engine(PipelineEngine* engine) { engine_ = engine; }

    Result<void, std::string> start(const std::string& socket_path, int max_clients) {
        socket_path_ = socket_path;
        ::unlink(socket_path.c_str());

        listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (listen_fd_ < 0) {
            return Result<void, std::string>::error(
                std::string("socket() failed: ") + ::strerror(errno));
        }

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

        if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            int e = errno;
            ::close(listen_fd_); listen_fd_ = -1;
            return Result<void, std::string>::error(
                std::string("bind() failed: ") + ::strerror(e));
        }

        if (::listen(listen_fd_, max_clients) < 0) {
            int e = errno;
            ::close(listen_fd_); listen_fd_ = -1;
            return Result<void, std::string>::error(
                std::string("listen() failed: ") + ::strerror(e));
        }

        int flags = ::fcntl(listen_fd_, F_GETFL, 0);
        if (flags >= 0) ::fcntl(listen_fd_, F_SETFL, flags | O_NONBLOCK);

        running_.store(true);
        accept_thread_ = std::thread([this]() { accept_loop(); });
        return Result<void, std::string>::ok();
    }

    void stop() {
        running_.store(false);
        if (listen_fd_ >= 0) { ::close(listen_fd_); listen_fd_ = -1; }
        if (accept_thread_.joinable()) accept_thread_.join();
        std::lock_guard lock(clients_mu_);
        for (int fd : client_fds_) ::close(fd);
        client_fds_.clear();
        if (!socket_path_.empty()) ::unlink(socket_path_.c_str());
    }

private:
    void accept_loop() {
        while (running_.load()) {
            struct pollfd pfd{};
            pfd.fd = listen_fd_;
            pfd.events = POLLIN;
            if (::poll(&pfd, 1, 500) <= 0) continue;

            int client_fd = ::accept(listen_fd_, nullptr, nullptr);
            if (client_fd < 0) continue;

            {
                std::lock_guard lock(clients_mu_);
                client_fds_.push_back(client_fd);
            }

            std::thread([this, client_fd]() { handle_client(client_fd); }).detach();
        }
    }

    void handle_client(int fd) {
        char buf[65536];
        std::string accumulated;

        while (running_.load()) {
            struct pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLIN;
            if (::poll(&pfd, 1, 1000) <= 0) continue;

            ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n <= 0) break;

            accumulated.append(buf, static_cast<size_t>(n));

            size_t pos;
            while ((pos = accumulated.find('\n')) != std::string::npos) {
                std::string message = accumulated.substr(0, pos);
                accumulated.erase(0, pos + 1);
                if (message.empty()) continue;

                nlohmann::json response;
                try {
                    auto request = nlohmann::json::parse(message);
                    response = handle_request(request);
                } catch (const nlohmann::json::parse_error& e) {
                    response["jsonrpc"] = "2.0";
                    response["error"]["code"] = -32700;
                    response["error"]["message"] = std::string("Parse error: ") + e.what();
                }

                std::string out = response.dump() + "\n";
                size_t total = 0;
                while (total < out.size()) {
                    ssize_t w = ::write(fd, out.data() + total, out.size() - total);
                    if (w <= 0) goto done;
                    total += static_cast<size_t>(w);
                }
            }
        }

    done:
        ::close(fd);
        std::lock_guard lock(clients_mu_);
        client_fds_.erase(
            std::remove(client_fds_.begin(), client_fds_.end(), fd),
            client_fds_.end());
    }

    nlohmann::json handle_request(const nlohmann::json& req) {
        nlohmann::json resp;
        resp["jsonrpc"] = "2.0";
        if (req.contains("id")) resp["id"] = req["id"];

        std::string method = req.value("method", "");
        auto params = req.value("params", nlohmann::json::object());

        if (method == "create") {
            std::string name = params.value("name", "");
            std::string spec = params.value("spec", "");
            auto json_def = params.value("definition", nlohmann::json());

            if (name.empty()) {
                resp["error"]["code"] = -32602;
                resp["error"]["message"] = "Missing 'name' parameter";
                return resp;
            }

            Result<void, std::string> result = Result<void, std::string>::error("No spec provided");
            if (!spec.empty()) {
                result = engine_->create_from_dsl(name, spec);
            } else if (!json_def.is_null()) {
                result = engine_->create_from_json(name, json_def);
            }

            if (result.has_value()) {
                resp["result"] = {{"status", "created"}, {"name", name}};
            } else {
                resp["error"]["code"] = -32000;
                resp["error"]["message"] = result.error();
            }

        } else if (method == "start") {
            std::string name = params.value("name", "");
            auto r = engine_->start_pipeline(name);
            if (r.has_value()) {
                resp["result"] = {{"status", "started"}, {"name", name}};
            } else {
                resp["error"]["code"] = -32000;
                resp["error"]["message"] = r.error();
            }

        } else if (method == "stop") {
            std::string name = params.value("name", "");
            auto r = engine_->stop_pipeline(name);
            if (r.has_value()) {
                resp["result"] = {{"status", "stopped"}, {"name", name}};
            } else {
                resp["error"]["code"] = -32000;
                resp["error"]["message"] = r.error();
            }

        } else if (method == "delete") {
            std::string name = params.value("name", "");
            auto r = engine_->delete_pipeline(name);
            if (r.has_value()) {
                resp["result"] = {{"status", "deleted"}, {"name", name}};
            } else {
                resp["error"]["code"] = -32000;
                resp["error"]["message"] = r.error();
            }

        } else if (method == "list") {
            auto pipelines = engine_->list_pipelines();
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& [name, state] : pipelines) {
                const char* state_str = "unknown";
                switch (state) {
                    case PipelineState::Created:  state_str = "created"; break;
                    case PipelineState::Starting: state_str = "starting"; break;
                    case PipelineState::Running:  state_str = "running"; break;
                    case PipelineState::Stopping: state_str = "stopping"; break;
                    case PipelineState::Stopped:  state_str = "stopped"; break;
                    case PipelineState::Error:    state_str = "error"; break;
                }
                arr.push_back({{"name", name}, {"state", state_str}});
            }
            resp["result"] = arr;

        } else if (method == "status") {
            std::string name = params.value("name", "");
            auto r = engine_->get_status(name);
            if (r.has_value()) {
                resp["result"] = r.value();
            } else {
                resp["error"]["code"] = -32000;
                resp["error"]["message"] = r.error();
            }

        } else if (method == "metrics") {
            std::string name = params.value("name", "");
            auto r = engine_->get_metrics(name);
            if (r.has_value()) {
                nlohmann::json arr = nlohmann::json::array();
                for (const auto& m : r.value()) {
                    arr.push_back({
                        {"node", m.instance_name},
                        {"type", m.node_type},
                        {"throughput_mbps", m.throughput_mbps},
                        {"latency_ms", m.latency_ms},
                        {"messages", m.messages_processed},
                        {"bytes", m.bytes_processed},
                        {"bottleneck", m.is_bottleneck}
                    });
                }
                resp["result"] = arr;
            } else {
                resp["error"]["code"] = -32000;
                resp["error"]["message"] = r.error();
            }

        } else if (method == "nodes") {
            auto nodes = engine_->registry().list();
            nlohmann::json arr = nlohmann::json::array();
            for (const auto* n : nodes) {
                arr.push_back({
                    {"name", n->name},
                    {"description", n->description},
                    {"input", n->input_type},
                    {"output", n->output_type},
                    {"builtin", n->is_builtin}
                });
            }
            resp["result"] = arr;

        } else {
            resp["error"]["code"] = -32601;
            resp["error"]["message"] = "Unknown method: " + method;
        }

        return resp;
    }

    PipelineEngine* engine_ = nullptr;
    int listen_fd_ = -1;
    std::string socket_path_;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    std::mutex clients_mu_;
    std::vector<int> client_fds_;
};

/// Weave daemon — manages service composition pipelines.
class WeaveDaemon : public DaemonBase {
public:
    Result<void, SLError> init(const Config& cfg) override {
        SL_INFO("weave: initializing daemon");

        tick_interval_ms_ = cfg.get<int>("daemon.tick_interval_ms", 1000);

        std::string socket_path = cfg.get<std::string>(
            "ipc.socket_path", "/run/straylight/weave.sock");
        int max_clients = cfg.get<int>("ipc.max_clients", 16);

        ipc_.set_engine(&engine_);
        auto ipc_result = ipc_.start(socket_path, max_clients);
        if (!ipc_result.has_value()) {
            SL_WARN("weave: IPC server failed to start: {}", ipc_result.error());
        }

        // Auto-create pipelines from config
        if (cfg.has("pipelines")) {
            auto raw = cfg.raw();
            if (raw.contains("pipelines") && raw["pipelines"].is_array()) {
                for (const auto& p : raw["pipelines"]) {
                    std::string name = p.value("name", "");
                    std::string spec = p.value("spec", "");
                    bool autostart = p.value("autostart", false);

                    if (!name.empty() && !spec.empty()) {
                        auto r = engine_.create_from_dsl(name, spec);
                        if (r.has_value()) {
                            SL_INFO("weave: created pipeline '{}'", name);
                            if (autostart) {
                                auto sr = engine_.start_pipeline(name);
                                if (sr.has_value()) {
                                    SL_INFO("weave: started pipeline '{}'", name);
                                } else {
                                    SL_WARN("weave: failed to start '{}': {}",
                                            name, sr.error());
                                }
                            }
                        } else {
                            SL_WARN("weave: failed to create '{}': {}", name, r.error());
                        }
                    }
                }
            }
        }

        SL_INFO("weave: daemon initialized (tick={}ms)", tick_interval_ms_);
        return Result<void, SLError>::ok();
    }

    Result<void, SLError> tick() override {
        std::this_thread::sleep_for(std::chrono::milliseconds(tick_interval_ms_));

        // Health check: verify running pipeline nodes are still alive
        auto pipelines = engine_.list_pipelines();
        for (const auto& [name, state] : pipelines) {
            if (state == PipelineState::Running) {
                auto status = engine_.get_status(name);
                if (status.has_value()) {
                    const auto& s = status.value();
                    if (s.contains("nodes")) {
                        for (const auto& node : s["nodes"]) {
                            if (node.contains("alive") && !node["alive"].get<bool>()) {
                                SL_WARN("weave: node '{}' in pipeline '{}' is dead",
                                        node.value("name", "?"), name);
                            }
                        }
                    }
                }
            }
        }

        return Result<void, SLError>::ok();
    }

    void shutdown() override {
        SL_INFO("weave: shutting down");

        // Stop all running pipelines
        auto pipelines = engine_.list_pipelines();
        for (const auto& [name, state] : pipelines) {
            if (state == PipelineState::Running) {
                SL_INFO("weave: stopping pipeline '{}'", name);
                engine_.stop_pipeline(name);
            }
        }

        ipc_.stop();
        SL_INFO("weave: shutdown complete");
    }

private:
    PipelineEngine engine_;
    WeaveIpcServer ipc_;
    int tick_interval_ms_ = 1000;
};

} // namespace straylight

int main(int /*argc*/, char* /*argv*/[]) {
    straylight::Log::init("straylight-weave");

    auto cfg_result = straylight::Config::load("/etc/straylight/weave.conf");
    if (!cfg_result.has_value()) {
        SL_ERROR("weave: failed to load config: {}", cfg_result.error());
        return 1;
    }

    straylight::WeaveDaemon daemon;
    return daemon.run(cfg_result.value());
}
