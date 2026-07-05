// lib/common/include/straylight/ipc_client.h
// Simple Unix socket JSON IPC client for widget<->daemon communication.
#pragma once

#include <straylight/result.h>
#include <nlohmann/json.hpp>

#include <cstring>
#include <string>
#include <string_view>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>

namespace straylight {

/// Lightweight JSON IPC client that connects to a StrayLight daemon socket
/// and exchanges newline-delimited JSON messages.
class IpcJsonClient {
public:
    IpcJsonClient() = default;

    ~IpcJsonClient() {
        disconnect();
    }

    IpcJsonClient(const IpcJsonClient&) = delete;
    IpcJsonClient& operator=(const IpcJsonClient&) = delete;

    IpcJsonClient(IpcJsonClient&& o) noexcept : fd_(o.fd_), connected_(o.connected_) {
        o.fd_ = -1;
        o.connected_ = false;
    }

    IpcJsonClient& operator=(IpcJsonClient&& o) noexcept {
        if (this != &o) {
            disconnect();
            fd_ = o.fd_;
            connected_ = o.connected_;
            o.fd_ = -1;
            o.connected_ = false;
        }
        return *this;
    }

    /// Connect to a daemon's Unix socket at the given path.
    /// Standard StrayLight sockets live under /run/straylight/<daemon>.sock
    Result<void, std::string> connect(const std::string& socket_path) {
        disconnect();

        fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd_ < 0) {
            return Result<void, std::string>::error(
                std::string("socket() failed: ") + ::strerror(errno));
        }

        // Set non-blocking for connect timeout
        int flags = ::fcntl(fd_, F_GETFL, 0);
        if (flags >= 0) {
            ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
        }

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

        int rc = ::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
        if (rc < 0 && errno != EINPROGRESS) {
            int e = errno;
            ::close(fd_);
            fd_ = -1;
            return Result<void, std::string>::error(
                std::string("connect() to ") + socket_path + " failed: " + ::strerror(e));
        }

        if (rc < 0) {
            // Wait for connect to complete (up to 2 seconds)
            struct pollfd pfd{};
            pfd.fd = fd_;
            pfd.events = POLLOUT;
            int pr = ::poll(&pfd, 1, 2000);
            if (pr <= 0) {
                ::close(fd_);
                fd_ = -1;
                return Result<void, std::string>::error("connect() timed out to " + socket_path);
            }
            int so_error = 0;
            socklen_t len = sizeof(so_error);
            ::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &so_error, &len);
            if (so_error != 0) {
                ::close(fd_);
                fd_ = -1;
                return Result<void, std::string>::error(
                    std::string("connect() async failed: ") + ::strerror(so_error));
            }
        }

        // Restore blocking mode for normal I/O
        if (flags >= 0) {
            ::fcntl(fd_, F_SETFL, flags);
        }

        connected_ = true;
        return Result<void, std::string>::ok();
    }

    void disconnect() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        connected_ = false;
    }

    [[nodiscard]] bool is_connected() const { return connected_; }

    /// Send a JSON request and receive a JSON response.
    /// Messages are newline-delimited JSON over the stream.
    Result<nlohmann::json, std::string> request(const nlohmann::json& msg) {
        if (!connected_) {
            return Result<nlohmann::json, std::string>::error("Not connected");
        }

        // Send
        std::string payload = msg.dump() + "\n";
        size_t total = 0;
        while (total < payload.size()) {
            ssize_t n = ::send(fd_, payload.data() + total, payload.size() - total, MSG_NOSIGNAL);
            if (n <= 0) {
                if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
                connected_ = false;
                return Result<nlohmann::json, std::string>::error(
                    std::string("write() failed: ") + ::strerror(errno));
            }
            total += static_cast<size_t>(n);
        }

        // Receive until newline
        std::string response;
        char buf[4096];
        while (true) {
            // Poll with 5 second timeout
            struct pollfd pfd{};
            pfd.fd = fd_;
            pfd.events = POLLIN;
            int pr = ::poll(&pfd, 1, 5000);
            if (pr <= 0) {
                return Result<nlohmann::json, std::string>::error("read() timed out");
            }

            ssize_t n = ::read(fd_, buf, sizeof(buf));
            if (n <= 0) {
                if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
                connected_ = false;
                return Result<nlohmann::json, std::string>::error("Connection closed");
            }
            response.append(buf, static_cast<size_t>(n));
            if (response.find('\n') != std::string::npos) break;
        }

        // Trim trailing newline
        if (!response.empty() && response.back() == '\n') {
            response.pop_back();
        }

        try {
            auto j = nlohmann::json::parse(response);
            return Result<nlohmann::json, std::string>::ok(std::move(j));
        } catch (const nlohmann::json::parse_error& e) {
            return Result<nlohmann::json, std::string>::error(
                std::string("JSON parse error: ") + e.what());
        }
    }

    /// Send a command string and get JSON response — convenience wrapper.
    Result<nlohmann::json, std::string> command(const std::string& cmd,
                                                 const nlohmann::json& params = {}) {
        nlohmann::json msg;
        msg["cmd"] = cmd;
        if (!params.empty()) {
            msg["params"] = params;
        }
        return request(msg);
    }


    /// JSON-RPC 2.0 call. Sends both "method" and "cmd" so strict and
    /// lenient StrayLight daemons both accept it. Returns the unwrapped
    /// "result" on success, or the daemon error message.
    Result<nlohmann::json, std::string> call(const std::string& method,
                                             const nlohmann::json& params = {}) {
        nlohmann::json msg;
        msg["jsonrpc"] = "2.0";
        msg["id"] = ++id_;
        msg["method"] = method;
        msg["cmd"] = method;
        if (!params.empty()) msg["params"] = params;
        auto res = request(msg);
        if (!res.has_value()) {
            return Result<nlohmann::json, std::string>::error(res.error());
        }
        const nlohmann::json& j = res.value();
        if (j.contains("error")) {
            std::string m = "daemon error";
            if (j["error"].is_object() && j["error"].contains("message"))
                m = j["error"]["message"].get<std::string>();
            else if (j["error"].is_string())
                m = j["error"].get<std::string>();
            return Result<nlohmann::json, std::string>::error(m);
        }
        if (j.contains("result")) {
            return Result<nlohmann::json, std::string>::ok(j["result"]);
        }
        return Result<nlohmann::json, std::string>::ok(j);
    }

private:
    int id_ = 0;
    int fd_ = -1;
    bool connected_ = false;
};

} // namespace straylight
