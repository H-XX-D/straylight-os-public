// services/flux/main.cpp
// straylight-flux — Realtime data stream processor daemon.
// Manages named streams with ring buffers, filtering, transforms, and IPC.
#include "stream_engine.h"
#include "filters.h"

#include <straylight/daemon.h>
#include <straylight/config.h>
#include <straylight/log.h>
#include <straylight/ipc_client.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <grp.h>

namespace straylight {

/// IPC server that handles JSON-RPC requests over a Unix domain socket.
class FluxIpcServer {
public:
    FluxIpcServer() = default;
    ~FluxIpcServer() { stop(); }

    void set_engine(StreamEngine* engine) { engine_ = engine; }

    Result<void, std::string> start(const std::string& socket_path, int max_clients) {
        socket_path_ = socket_path;
        max_clients_ = max_clients;

        // Remove stale socket
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
            ::close(listen_fd_);
            listen_fd_ = -1;
            return Result<void, std::string>::error(
                std::string("bind() failed: ") + ::strerror(e));
        }

        if (::listen(listen_fd_, max_clients) < 0) {
            int e = errno;
            ::close(listen_fd_);
            listen_fd_ = -1;
            return Result<void, std::string>::error(
                std::string("listen() failed: ") + ::strerror(e));
        }

        if (auto* group = ::getgrnam("straylight")) {
            ::chown(socket_path.c_str(), 0, group->gr_gid);
        }
        ::chmod(socket_path.c_str(), 0660);

        // Set non-blocking
        int flags = ::fcntl(listen_fd_, F_GETFL, 0);
        if (flags >= 0) {
            ::fcntl(listen_fd_, F_SETFL, flags | O_NONBLOCK);
        }

        running_.store(true);
        accept_thread_ = std::thread([this]() { accept_loop(); });
        return Result<void, std::string>::ok();
    }

    void stop() {
        running_.store(false);
        if (listen_fd_ >= 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        if (accept_thread_.joinable()) {
            accept_thread_.join();
        }
        // Close all client connections
        std::lock_guard lock(clients_mu_);
        for (int fd : client_fds_) {
            ::close(fd);
        }
        client_fds_.clear();

        if (!socket_path_.empty()) {
            ::unlink(socket_path_.c_str());
        }
    }

private:
    void accept_loop() {
        while (running_.load()) {
            struct pollfd pfd{};
            pfd.fd = listen_fd_;
            pfd.events = POLLIN;
            int pr = ::poll(&pfd, 1, 500);
            if (pr <= 0) continue;

            int client_fd = ::accept(listen_fd_, nullptr, nullptr);
            if (client_fd < 0) continue;

            {
                std::lock_guard lock(clients_mu_);
                if (static_cast<int>(client_fds_.size()) >= max_clients_) {
                    ::close(client_fd);
                    continue;
                }
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
            int pr = ::poll(&pfd, 1, 1000);
            if (pr < 0) break;
            if (pr == 0) continue;

            ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n <= 0) break;

            accumulated.append(buf, static_cast<size_t>(n));

            // Process newline-delimited messages
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
                    if (w <= 0) {
                        if (w < 0 && (errno == EINTR || errno == EAGAIN)) continue;
                        goto done;
                    }
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
            size_t buffer = params.value("buffer", 1000);
            if (name.empty()) {
                resp["error"]["code"] = -32602;
                resp["error"]["message"] = "Missing 'name' parameter";
                return resp;
            }
            auto r = engine_->create_stream(name, buffer);
            if (r.has_value()) {
                resp["result"] = {{"status", "created"}, {"name", name}, {"buffer", buffer}};
            } else {
                resp["error"]["code"] = -32000;
                resp["error"]["message"] = r.error().message();
            }

        } else if (method == "delete") {
            std::string name = params.value("name", "");
            auto r = engine_->delete_stream(name);
            if (r.has_value()) {
                resp["result"] = {{"status", "deleted"}, {"name", name}};
            } else {
                resp["error"]["code"] = -32000;
                resp["error"]["message"] = r.error().message();
            }

        } else if (method == "publish") {
            std::string stream = params.value("stream", "");
            auto payload = params.value("payload", nlohmann::json::object());
            auto r = engine_->publish(stream, payload);
            if (r.has_value()) {
                resp["result"] = {{"status", "published"}};
            } else {
                resp["error"]["code"] = -32000;
                resp["error"]["message"] = r.error().message();
            }

        } else if (method == "replay") {
            std::string stream = params.value("stream", "");
            size_t count = params.value("count", 10);
            auto r = engine_->replay(stream, count);
            if (r.has_value()) {
                nlohmann::json events = nlohmann::json::array();
                for (const auto& ev : r.value()) {
                    nlohmann::json e;
                    e["sequence"] = ev.sequence;
                    e["payload"] = ev.payload;
                    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        ev.wall_timestamp.time_since_epoch()).count();
                    auto monotonic_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        ev.timestamp.time_since_epoch()).count();
                    e["timestamp_ms"] = ms;
                    e["monotonic_ms"] = monotonic_ms;
                    events.push_back(e);
                }
                resp["result"] = events;
            } else {
                resp["error"]["code"] = -32000;
                resp["error"]["message"] = r.error().message();
            }

        } else if (method == "list") {
            nlohmann::json streams = nlohmann::json::array();
            for (const auto& info : engine_->list_streams()) {
                nlohmann::json s;
                s["name"] = info.name;
                s["buffer_capacity"] = info.buffer_capacity;
                s["total_published"] = info.total_published;
                s["subscriber_count"] = info.subscriber_count;
                streams.push_back(s);
            }
            resp["result"] = streams;

        } else if (method == "info") {
            std::string name = params.value("name", "");
            auto r = engine_->stream_info(name);
            if (r.has_value()) {
                const auto& info = r.value();
                resp["result"] = {
                    {"name", info.name},
                    {"buffer_capacity", info.buffer_capacity},
                    {"total_published", info.total_published},
                    {"subscriber_count", info.subscriber_count}
                };
            } else {
                resp["error"]["code"] = -32000;
                resp["error"]["message"] = r.error().message();
            }

        } else if (method == "transform") {
            std::string stream = params.value("stream", "");
            std::string path = params.value("path", "");
            size_t count = params.value("count", 10);

            auto r = engine_->replay(stream, count);
            if (!r.has_value()) {
                resp["error"]["code"] = -32000;
                resp["error"]["message"] = r.error().message();
                return resp;
            }

            nlohmann::json results = nlohmann::json::array();
            for (const auto& ev : r.value()) {
                auto extracted = TransformEngine::extract(ev.payload, path);
                if (extracted.has_value()) {
                    results.push_back(extracted.value());
                }
            }
            resp["result"] = results;

        } else if (method == "aggregate") {
            std::string stream = params.value("stream", "");
            std::string field = params.value("field", "");
            std::string type_str = params.value("type", "avg");
            size_t window = params.value("window", 10);

            auto agg_type = TransformEngine::parse_aggregation_type(type_str);
            if (!agg_type.has_value()) {
                resp["error"]["code"] = -32602;
                resp["error"]["message"] = agg_type.error().message();
                return resp;
            }

            auto r = engine_->replay(stream, window);
            if (!r.has_value()) {
                resp["error"]["code"] = -32000;
                resp["error"]["message"] = r.error().message();
                return resp;
            }

            std::vector<nlohmann::json> payloads;
            for (const auto& ev : r.value()) {
                payloads.push_back(ev.payload);
            }

            AggregationSpec spec;
            spec.field = field;
            spec.type = agg_type.value();
            spec.window_size = window;

            auto agg_result = TransformEngine::aggregate(payloads, spec);
            if (agg_result.has_value()) {
                resp["result"] = {
                    {"field", field},
                    {"type", type_str},
                    {"window", window},
                    {"value", agg_result.value()},
                    {"samples", payloads.size()}
                };
            } else {
                resp["error"]["code"] = -32000;
                resp["error"]["message"] = agg_result.error().message();
            }

        } else {
            resp["error"]["code"] = -32601;
            resp["error"]["message"] = "Unknown method: " + method;
        }

        return resp;
    }

    StreamEngine* engine_ = nullptr;
    int listen_fd_ = -1;
    std::string socket_path_;
    int max_clients_ = 8;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    std::mutex clients_mu_;
    std::vector<int> client_fds_;
};

/// Flux daemon — manages named data streams with filtering and transforms.
class FluxDaemon : public DaemonBase {
public:
    Result<void, SLError> init(const Config& cfg) override {
        SL_INFO("flux: initializing daemon");

        // Load default streams from config
        if (cfg.has("streams")) {
            auto raw = cfg.raw();
            if (raw.contains("streams") && raw["streams"].is_array()) {
                for (const auto& s : raw["streams"]) {
                    std::string name = s.value("name", "");
                    size_t buffer = s.value("buffer", 1000);
                    if (!name.empty()) {
                        auto r = engine_.create_stream(name, buffer);
                        if (!r.has_value()) {
                            SL_WARN("flux: failed to create default stream '{}': {}",
                                    name, r.error().message());
                        }
                    }
                }
            }
        }

        // IPC
        std::string socket_path = cfg.get<std::string>(
            "ipc.socket_path", "/run/straylight/flux.sock");
        int max_clients = cfg.get<int>("ipc.max_clients", 16);

        ipc_.set_engine(&engine_);
        auto ipc_result = ipc_.start(socket_path, max_clients);
        if (!ipc_result.has_value()) {
            SL_WARN("flux: IPC server failed to start: {}", ipc_result.error());
        }

        tick_interval_ms_ = cfg.get<int>("daemon.tick_interval_ms", 100);

        SL_INFO("flux: daemon initialized (tick={}ms)", tick_interval_ms_);
        return Result<void, SLError>::ok();
    }

    Result<void, SLError> tick() override {
        // The main loop just sleeps; real work happens in IPC handler threads
        std::this_thread::sleep_for(std::chrono::milliseconds(tick_interval_ms_));
        return Result<void, SLError>::ok();
    }

    void shutdown() override {
        SL_INFO("flux: shutting down");
        ipc_.stop();
        SL_INFO("flux: shutdown complete");
    }

private:
    StreamEngine engine_;
    FluxIpcServer ipc_;
    int tick_interval_ms_ = 100;
};

} // namespace straylight

int main(int /*argc*/, char* /*argv*/[]) {
    straylight::Log::init("straylight-flux");

    auto cfg_result = straylight::Config::load("/etc/straylight/flux.conf");
    if (!cfg_result.has_value()) {
        SL_ERROR("flux: failed to load config: {}", cfg_result.error());
        return 1;
    }

    straylight::FluxDaemon daemon;
    return daemon.run(cfg_result.value());
}
