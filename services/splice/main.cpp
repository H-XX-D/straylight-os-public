// services/splice/main.cpp
// straylight-splice daemon — Zero-copy pipeline stitching via VPU slab allocator.
// Manages splice sessions between processes over Unix socket IPC.

#include "splice_engine.h"

#include <straylight/config.h>
#include <straylight/daemon.h>
#include <straylight/log.h>
#include <straylight/ipc.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

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
    auto send_result = conn.send(response.dump());
    if (!send_result.has_value()) {
        SL_WARN("splice: failed to send response: {}", send_result.error());
    }
}

} // namespace

class SpliceDaemon : public DaemonBase {
public:
    Result<void, SLError> init(const Config& cfg) override {
        SL_INFO("splice: initializing daemon");

        socket_path_ = cfg.get<std::string>(
            "ipc.socket_path", "/run/straylight/splice.sock");
        tick_interval_ms_ = cfg.get<int>("tick_interval_ms", 500);
        default_splice_size_ = cfg.get<uint64_t>("default_splice_size", 1048576);

        engine_ = std::make_unique<SpliceEngine>();

        auto bind_result = server_.bind(socket_path_);
        if (!bind_result.has_value()) {
            return Result<void, SLError>::error(
                SLError{SLErrorCode::IpcFailed,
                        "Failed to bind socket: " + bind_result.error()});
        }

        SL_INFO("splice: listening on {}", socket_path_);
        SL_INFO("splice: daemon initialized (tick={}ms, default_size={})",
                tick_interval_ms_, default_splice_size_);

        return Result<void, SLError>::ok();
    }

    Result<void, SLError> tick() override {
        // Accept new connections (non-blocking)
        auto conn_result = server_.accept(tick_interval_ms_);
        if (conn_result.has_value()) {
            handle_connection(std::move(conn_result).value());
        }

        // Update metrics for all active sessions
        engine_->update_metrics();

        return Result<void, SLError>::ok();
    }

    void shutdown() override {
        SL_INFO("splice: shutting down, destroying all sessions");
        engine_.reset();
        SL_INFO("splice: shutdown complete");
    }

private:
    void handle_connection(std::unique_ptr<IpcConnection> conn) {
        set_receive_timeout(*conn);
        auto msg_result = conn->receive();
        if (!msg_result.has_value()) {
            if (msg_result.error() == "Failed to receive message length") {
                SL_DEBUG("splice: client closed before sending a request");
            } else {
                SL_WARN("splice: failed to receive message: {}", msg_result.error());
            }
            return;
        }

        nlohmann::json request;
        try {
            request = nlohmann::json::parse(msg_result.value());
        } catch (const nlohmann::json::parse_error& e) {
            nlohmann::json err_resp;
            err_resp["error"] = std::string("JSON parse error: ") + e.what();
            send_response(*conn, err_resp);
            return;
        }

        nlohmann::json response = dispatch(request);
        send_response(*conn, response);
    }

    nlohmann::json dispatch(const nlohmann::json& request) {
        std::string method = request.value("method", "");
        nlohmann::json params = request.value("params", nlohmann::json::object());

        nlohmann::json response;
        response["jsonrpc"] = "2.0";
        response["id"] = request.value("id", 0);

        if (method == "create") {
            return handle_create(params, response);
        } else if (method == "destroy") {
            return handle_destroy(params, response);
        } else if (method == "list") {
            return handle_list(response);
        } else if (method == "stats") {
            return handle_stats(params, response);
        } else {
            response["error"] = "Unknown method: " + method;
        }

        return response;
    }

    nlohmann::json handle_create(const nlohmann::json& params, nlohmann::json response) {
        pid_t producer = params.value("producer_pid", 0);
        pid_t consumer = params.value("consumer_pid", 0);
        uint64_t size = params.value("size", default_splice_size_);

        auto result = engine_->create_splice(producer, consumer, size);
        if (result.has_value()) {
            response["result"]["session_id"] = result.value();
            response["result"]["size"] = size;
            response["result"]["status"] = "created";
        } else {
            response["error"]["code"] = static_cast<int>(result.error().code());
            response["error"]["message"] = result.error().message();
        }
        return response;
    }

    nlohmann::json handle_destroy(const nlohmann::json& params, nlohmann::json response) {
        uint64_t session_id = params.value("session_id", uint64_t{0});

        auto result = engine_->destroy_splice(session_id);
        if (result.has_value()) {
            response["result"]["status"] = "destroyed";
            response["result"]["session_id"] = session_id;
        } else {
            response["error"]["code"] = static_cast<int>(result.error().code());
            response["error"]["message"] = result.error().message();
        }
        return response;
    }

    nlohmann::json handle_list(nlohmann::json response) {
        auto sessions = engine_->list_splices();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& s : sessions) {
            nlohmann::json entry;
            entry["session_id"] = s.session_id;
            entry["producer_pid"] = s.producer_pid;
            entry["consumer_pid"] = s.consumer_pid;
            entry["region_name"] = s.region_name;
            entry["size"] = s.size;
            entry["slab_order"] = s.slab.slab_order;
            entry["bytes_transferred"] = s.bytes_transferred;
            entry["throughput_mbps"] = s.throughput_mbps;
            auto now = std::chrono::steady_clock::now();
            entry["uptime_seconds"] = std::chrono::duration<double>(
                now - s.created_at).count();
            arr.push_back(entry);
        }
        response["result"] = arr;
        return response;
    }

    nlohmann::json handle_stats(const nlohmann::json& params, nlohmann::json response) {
        uint64_t session_id = params.value("session_id", uint64_t{0});

        auto result = engine_->get_stats(session_id);
        if (result.has_value()) {
            const auto& stats = result.value();
            response["result"]["session_id"] = stats.session_id;
            response["result"]["bytes_transferred"] = stats.bytes_transferred;
            response["result"]["push_count"] = stats.push_count;
            response["result"]["pop_count"] = stats.pop_count;
            response["result"]["throughput_mbps"] = stats.throughput_mbps;
            response["result"]["avg_latency_us"] = stats.avg_latency_us;
            response["result"]["ring_available"] = stats.ring_available;
            response["result"]["ring_capacity"] = stats.ring_capacity;
            response["result"]["fill_ratio"] = stats.fill_ratio;
            response["result"]["uptime_seconds"] = stats.uptime_seconds;
        } else {
            response["error"]["code"] = static_cast<int>(result.error().code());
            response["error"]["message"] = result.error().message();
        }
        return response;
    }

    std::unique_ptr<SpliceEngine> engine_;
    IpcServer server_;
    std::string socket_path_;
    int tick_interval_ms_{500};
    uint64_t default_splice_size_{1048576};
};

} // namespace straylight

int main(int /*argc*/, char* /*argv*/[]) {
    straylight::Log::init("straylight-splice");

    auto cfg_result = straylight::Config::load("/etc/straylight/splice.conf");
    if (!cfg_result.has_value()) {
        SL_WARN("splice: no config file found, using defaults");
        // Create minimal default config
        nlohmann::json defaults;
        defaults["ipc"]["socket_path"] = "/run/straylight/splice.sock";
        defaults["tick_interval_ms"] = 500;
        defaults["default_splice_size"] = 1048576;
        auto default_cfg = straylight::Config::load("/dev/null");
        if (!default_cfg.has_value()) {
            SL_ERROR("splice: cannot create default config");
            return 1;
        }
        straylight::SpliceDaemon daemon;
        return daemon.run(default_cfg.value());
    }

    straylight::SpliceDaemon daemon;
    return daemon.run(cfg_result.value());
}
