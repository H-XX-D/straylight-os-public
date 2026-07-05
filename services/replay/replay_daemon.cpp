// services/replay/replay_daemon.cpp
// Daemon implementation for the event flight recorder.
#include "replay_daemon.h"

#include <straylight/log.h>

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

namespace straylight {

Result<void, SLError> ReplayDaemon::init(const Config& cfg) {
    buffer_size_mb_ = cfg.get<int>("buffer.size_mb", 256);
    socket_path_ = cfg.get<std::string>("ipc.socket_path",
                                         "/run/straylight/replay.sock");

    auto start_result = recorder_.start(buffer_size_mb_);
    if (!start_result.has_value()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::Internal, start_result.error()});
    }

    auto ipc_result = setup_ipc();
    if (!ipc_result.has_value()) {
        SL_WARN("replay: IPC setup failed: {}", ipc_result.error().message());
        // Non-fatal: daemon can still record events
    }

    SL_INFO("replay: daemon initialized, buffer {}MB", buffer_size_mb_);
    return Result<void, SLError>::ok();
}

Result<void, SLError> ReplayDaemon::tick() {
    // Check for incoming IPC connections
    if (ipc_fd_ >= 0) {
        struct pollfd pfd{};
        pfd.fd = ipc_fd_;
        pfd.events = POLLIN;

        int ret = ::poll(&pfd, 1, 1000);  // 1 second poll
        if (ret > 0 && (pfd.revents & POLLIN)) {
            int client_fd = ::accept(ipc_fd_, nullptr, nullptr);
            if (client_fd >= 0) {
                handle_ipc_request(client_fd);
                ::close(client_fd);
            }
        }
    } else {
        // No IPC, just sleep
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return Result<void, SLError>::ok();
}

void ReplayDaemon::shutdown() {
    SL_INFO("replay: shutting down");
    recorder_.stop();

    if (ipc_fd_ >= 0) {
        ::close(ipc_fd_);
        ipc_fd_ = -1;
        ::unlink(socket_path_.c_str());
    }
}

Result<void, SLError> ReplayDaemon::setup_ipc() {
    // Ensure runtime directory exists
    std::string dir = socket_path_.substr(0, socket_path_.rfind('/'));
    ::mkdir(dir.c_str(), 0755);

    // Remove stale socket
    ::unlink(socket_path_.c_str());

    ipc_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (ipc_fd_ < 0) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError,
                    std::string("socket() failed: ") + ::strerror(errno)});
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(ipc_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        int e = errno;
        ::close(ipc_fd_);
        ipc_fd_ = -1;
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError,
                    std::string("bind() failed: ") + ::strerror(e)});
    }

    if (::listen(ipc_fd_, 4) < 0) {
        int e = errno;
        ::close(ipc_fd_);
        ipc_fd_ = -1;
        ::unlink(socket_path_.c_str());
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError,
                    std::string("listen() failed: ") + ::strerror(e)});
    }

    // Set socket permissions
    ::chmod(socket_path_.c_str(), 0660);

    SL_INFO("replay: IPC listening on {}", socket_path_);
    return Result<void, SLError>::ok();
}

void ReplayDaemon::handle_ipc_request(int client_fd) {
    // Read request (newline-delimited JSON)
    char buf[8192];
    std::string request;

    struct pollfd pfd{};
    pfd.fd = client_fd;
    pfd.events = POLLIN;

    int ret = ::poll(&pfd, 1, 2000);
    if (ret <= 0) return;

    ssize_t n = ::read(client_fd, buf, sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = '\0';
    request = buf;

    // Strip trailing newline
    while (!request.empty() && (request.back() == '\n' || request.back() == '\r')) {
        request.pop_back();
    }

    // Dispatch and send response
    std::string response = dispatch_rpc(request);
    response += "\n";

    size_t total = 0;
    while (total < response.size()) {
        ssize_t w = ::write(client_fd, response.data() + total,
                            response.size() - total);
        if (w <= 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            break;
        }
        total += static_cast<size_t>(w);
    }
}

std::string ReplayDaemon::dispatch_rpc(const std::string& request_json) {
    nlohmann::json response;
    response["jsonrpc"] = "2.0";
    response["id"] = 1;

    nlohmann::json req;
    try {
        req = nlohmann::json::parse(request_json);
    } catch (...) {
        response["error"] = {{"code", -32700}, {"message", "Parse error"}};
        return response.dump();
    }

    std::string method = req.value("method", "");
    auto params = req.value("params", nlohmann::json::object());

    if (req.contains("id")) {
        response["id"] = req["id"];
    }

    EventAnalyzer analyzer(recorder_);

    if (method == "status") {
        nlohmann::json result;
        result["running"] = recorder_.is_running();
        result["event_count"] = recorder_.event_count();
        result["buffer_path"] = recorder_.buffer_path();
        result["buffer_size_mb"] = buffer_size_mb_;
        response["result"] = result;

    } else if (method == "last") {
        uint64_t seconds = params.value("seconds", 300);
        auto tl = analyzer.build_timeline_last(seconds);
        if (tl.has_value()) {
            auto fmt = analyzer.format_timeline(tl.value());
            response["result"] = fmt.has_value() ? fmt.value() : "Format error";
        } else {
            response["error"] = {{"code", -32000}, {"message", tl.error()}};
        }

    } else if (method == "crash") {
        auto report = analyzer.analyze_crash();
        if (report.has_value()) {
            response["result"] = report.value().summary;
        } else {
            response["error"] = {{"code", -32000}, {"message", report.error()}};
        }

    } else if (method == "timeline") {
        uint64_t from = params.value("from_ns", uint64_t(0));
        uint64_t to = params.value("to_ns", uint64_t(0));
        std::string type_filter = params.value("type", "");
        int pid_filter = params.value("pid", -1);

        if (from == 0 || to == 0) {
            // Default to last 5 minutes
            auto tl = analyzer.build_timeline_last(300);
            if (tl.has_value()) {
                auto fmt = analyzer.format_timeline(tl.value());
                response["result"] = fmt.has_value() ? fmt.value() : "Format error";
            } else {
                response["error"] = {{"code", -32000}, {"message", tl.error()}};
            }
        } else {
            auto tl = analyzer.build_timeline(from, to);
            if (tl.has_value()) {
                auto fmt = analyzer.format_timeline(tl.value());
                response["result"] = fmt.has_value() ? fmt.value() : "Format error";
            } else {
                response["error"] = {{"code", -32000}, {"message", tl.error()}};
            }
        }

    } else if (method == "search") {
        std::string pattern = params.value("pattern", "");
        auto results = analyzer.search(pattern);
        if (results.has_value()) {
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& ev : results.value()) {
                nlohmann::json obj;
                obj["timestamp_ns"] = ev.timestamp_ns;
                obj["type"] = event_type_name(ev.type);
                obj["pid"] = ev.pid;
                obj["process_name"] = ev.process_name;
                obj["detail"] = ev.detail;
                arr.push_back(std::move(obj));
            }
            response["result"] = arr;
        } else {
            response["error"] = {{"code", -32000}, {"message", results.error()}};
        }

    } else if (method == "export") {
        uint64_t seconds = params.value("seconds", 300);
        auto tl = analyzer.build_timeline_last(seconds);
        if (tl.has_value()) {
            auto json_str = analyzer.export_json(tl.value());
            response["result"] = json_str.has_value() ? json_str.value() : "{}";
        } else {
            response["error"] = {{"code", -32000}, {"message", tl.error()}};
        }

    } else if (method == "watch") {
        // Return latest 10 events as a snapshot for watch mode
        auto all = recorder_.snapshot();
        nlohmann::json arr = nlohmann::json::array();
        size_t start_idx = (all.size() > 10) ? (all.size() - 10) : 0;
        for (size_t i = start_idx; i < all.size(); ++i) {
            nlohmann::json obj;
            obj["timestamp_ns"] = all[i].timestamp_ns;
            obj["type"] = event_type_name(all[i].type);
            obj["pid"] = all[i].pid;
            obj["process_name"] = all[i].process_name;
            obj["detail"] = all[i].detail;
            arr.push_back(std::move(obj));
        }
        response["result"] = arr;

    } else {
        response["error"] = {{"code", -32601}, {"message", "Method not found: " + method}};
    }

    return response.dump();
}

} // namespace straylight
