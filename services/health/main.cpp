// services/health/main.cpp
// straylight-health daemon - Continuous system health monitoring.
#include "health_scorer.h"
#include "checks.h"

#include <straylight/config.h>
#include <straylight/daemon.h>
#include <straylight/log.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <grp.h>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace straylight {

namespace {

enum class ClientProtocol {
    LengthPrefixed,
    NewlineJson,
};

struct ClientRequest {
    ClientProtocol protocol = ClientProtocol::LengthPrefixed;
    nlohmann::json body;
};

const char* status_string(HealthStatus status) {
    switch (status) {
        case HealthStatus::Ok: return "ok";
        case HealthStatus::Warn: return "warn";
        case HealthStatus::Critical: return "critical";
    }
    return "unknown";
}

nlohmann::json check_to_json(const CheckResult& check) {
    return {
        {"name", check.name},
        {"score", check.score},
        {"status", status_string(check.status)},
        {"detail", check.detail},
        {"weight", check.weight},
    };
}

nlohmann::json snapshot_to_json(const HealthSnapshot& snap) {
    nlohmann::json checks = nlohmann::json::array();
    for (const auto& check : snap.checks) {
        checks.push_back(check_to_json(check));
    }

    return {
        {"timestamp", snap.timestamp},
        {"overall_score", snap.overall_score},
        {"overall_status", status_string(snap.overall_status)},
        {"checks", checks},
    };
}

bool send_all(int fd, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool recv_all(int fd, char* data, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = ::recv(fd, data + got, len - got, MSG_WAITALL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        got += static_cast<size_t>(n);
    }
    return true;
}

} // namespace

/// Health monitoring daemon.
class HealthDaemon : public DaemonBase {
public:
    Result<void, SLError> init(const Config& cfg) override {
        SL_INFO("health: initializing daemon");

        if (cfg.has("daemon.tick_interval_ms")) {
            check_interval_ms_ = cfg.get<int>("daemon.tick_interval_ms", check_interval_ms_);
        } else if (cfg.has("health.check_interval_s")) {
            check_interval_ms_ = cfg.get<int>("health.check_interval_s", 30) * 1000;
        } else {
            check_interval_ms_ = cfg.get<int>("tick_interval_seconds", 60) * 1000;
        }
        check_interval_ms_ = std::max(1000, check_interval_ms_);

        socket_path_ = cfg.get<std::string>(
            "health.socket",
            cfg.get<std::string>("ipc.socket_path", "/run/straylight/health.sock"));

        load_thresholds(cfg);

        auto setup_res = setup_ipc();
        if (!setup_res.has_value()) {
            return setup_res;
        }

        run_health_check(true);
        next_check_ = std::chrono::steady_clock::now() +
                      std::chrono::milliseconds(check_interval_ms_);

        SL_INFO("health: daemon initialized (check_interval={}ms, socket={})",
                check_interval_ms_, socket_path_);
        return Result<void, SLError>::ok();
    }

    Result<void, SLError> tick() override {
        poll_ipc(100);

        auto now = std::chrono::steady_clock::now();
        if (now >= next_check_) {
            run_health_check(false);
            next_check_ = now + std::chrono::milliseconds(check_interval_ms_);
        }

        return Result<void, SLError>::ok();
    }

    void shutdown() override {
        SL_INFO("health: shutting down");
        teardown_ipc();
        SL_INFO("health: shutdown complete");
    }

private:
    HealthScorer scorer_;
    std::string socket_path_ = "/run/straylight/health.sock";
    int check_interval_ms_ = 5000;
    std::chrono::steady_clock::time_point next_check_;

    int server_fd_ = -1;
    int epoll_fd_ = -1;
    std::vector<int> client_fds_;

    void load_thresholds(const Config& cfg) {
        if (!cfg.has("thresholds")) return;

        const auto& raw = cfg.raw();
        if (!raw.contains("thresholds") || !raw["thresholds"].is_object()) return;

        for (auto& [name, val] : raw["thresholds"].items()) {
            if (!val.is_object()) continue;
            int warn = val.value("warn_below", 70);
            int crit = val.value("critical_below", 30);
            scorer_.set_threshold(name, warn, crit);
        }
    }

    void run_health_check(bool initial) {
        auto prev = scorer_.latest();
        auto checks = HealthChecks::run_all();
        auto snap = scorer_.score(checks);

        if (initial) {
            SL_INFO("health: initial score = {} ({})",
                    snap.overall_score,
                    HealthScorer::status_string(snap.overall_status));
        } else if (prev.overall_score >= 0 &&
                   snap.overall_status != prev.overall_status) {
            if (snap.overall_status == HealthStatus::Critical) {
                SL_ERROR("health: score dropped to {} - CRITICAL", snap.overall_score);
            } else if (snap.overall_status == HealthStatus::Warn) {
                SL_WARN("health: score at {} - WARNING", snap.overall_score);
            } else {
                SL_INFO("health: recovered to {} - OK", snap.overall_score);
            }
        }

        for (const auto& cr : checks) {
            if (cr.status == HealthStatus::Critical) {
                SL_WARN("health: {} is CRITICAL: {}", cr.name, cr.detail);
            }
        }
    }

    Result<void, SLError> setup_ipc() {
        std::error_code ec;
        std::filesystem::create_directories(
            std::filesystem::path(socket_path_).parent_path(), ec);

        ::unlink(socket_path_.c_str());

        server_fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (server_fd_ < 0) {
            return Result<void, SLError>::error(
                SLError{SLErrorCode::IOError,
                        std::string("socket(): ") + std::strerror(errno)});
        }

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

        if (::bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            auto err = std::string("bind(): ") + std::strerror(errno);
            ::close(server_fd_);
            server_fd_ = -1;
            return Result<void, SLError>::error(SLError{SLErrorCode::IOError, err});
        }

        if (auto* group = ::getgrnam("straylight")) {
            ::chown(socket_path_.c_str(), 0, group->gr_gid);
            ::chmod(socket_path_.c_str(), 0660);
        } else {
            ::chmod(socket_path_.c_str(), 0600);
        }

        if (::listen(server_fd_, SOMAXCONN) < 0) {
            auto err = std::string("listen(): ") + std::strerror(errno);
            ::close(server_fd_);
            server_fd_ = -1;
            ::unlink(socket_path_.c_str());
            return Result<void, SLError>::error(SLError{SLErrorCode::IOError, err});
        }

        epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd_ < 0) {
            auto err = std::string("epoll_create1(): ") + std::strerror(errno);
            ::close(server_fd_);
            server_fd_ = -1;
            ::unlink(socket_path_.c_str());
            return Result<void, SLError>::error(SLError{SLErrorCode::IOError, err});
        }

        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = server_fd_;
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, server_fd_, &ev);

        SL_INFO("health: IPC listening on {}", socket_path_);
        return Result<void, SLError>::ok();
    }

    void teardown_ipc() {
        for (int fd : client_fds_) ::close(fd);
        client_fds_.clear();

        if (epoll_fd_ >= 0) {
            ::close(epoll_fd_);
            epoll_fd_ = -1;
        }
        if (server_fd_ >= 0) {
            ::close(server_fd_);
            server_fd_ = -1;
        }
        ::unlink(socket_path_.c_str());
    }

    void poll_ipc(int timeout_ms) {
        if (epoll_fd_ < 0) return;

        epoll_event events[16];
        int n = ::epoll_wait(epoll_fd_, events, 16, timeout_ms);
        for (int i = 0; i < n; ++i) {
            if (events[i].data.fd == server_fd_) {
                handle_accept();
            } else {
                handle_client(events[i].data.fd);
            }
        }
    }

    void handle_accept() {
        while (true) {
            sockaddr_un addr{};
            socklen_t len = sizeof(addr);
            int fd = ::accept4(server_fd_, reinterpret_cast<sockaddr*>(&addr),
                               &len, SOCK_CLOEXEC);
            if (fd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                SL_WARN("health: accept() failed: {}", std::strerror(errno));
                return;
            }

            epoll_event ev{};
            ev.events = EPOLLIN | EPOLLHUP | EPOLLERR;
            ev.data.fd = fd;
            ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev);
            client_fds_.push_back(fd);
        }
    }

    void handle_client(int fd) {
        auto req = recv_request(fd);
        if (!req) {
            remove_client(fd);
            return;
        }

        nlohmann::json response;
        try {
            response = dispatch(req->body);
        } catch (const std::exception& e) {
            response = error_response(req->body.value("id", nlohmann::json(nullptr)),
                                      std::string("internal error: ") + e.what());
        }

        if (!send_response(fd, req->protocol, response)) {
            remove_client(fd);
        }
    }

    void remove_client(int fd) {
        if (epoll_fd_ >= 0) ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
        ::close(fd);
        client_fds_.erase(
            std::remove(client_fds_.begin(), client_fds_.end(), fd),
            client_fds_.end());
    }

    std::optional<ClientRequest> recv_request(int fd) {
        unsigned char prefix[4]{};
        ssize_t n = ::recv(fd, prefix, sizeof(prefix), MSG_PEEK);
        if (n <= 0) return std::nullopt;

        if (prefix[0] == '{' || prefix[0] == '[') {
            std::string payload;
            char buf[4096];
            while (payload.find('\n') == std::string::npos &&
                   payload.size() < 1024 * 1024) {
                ssize_t got = ::recv(fd, buf, sizeof(buf), 0);
                if (got < 0 && errno == EINTR) continue;
                if (got <= 0) return std::nullopt;
                payload.append(buf, static_cast<size_t>(got));
            }
            auto newline = payload.find('\n');
            if (newline != std::string::npos) payload.resize(newline);
            return ClientRequest{ClientProtocol::NewlineJson,
                                 nlohmann::json::parse(payload)};
        }

        uint32_t len_le = 0;
        if (!recv_all(fd, reinterpret_cast<char*>(&len_le), sizeof(len_le))) {
            return std::nullopt;
        }
        uint32_t len = len_le;
        if (len == 0 || len > 4 * 1024 * 1024) {
            return std::nullopt;
        }

        std::string payload(len, '\0');
        if (!recv_all(fd, payload.data(), payload.size())) {
            return std::nullopt;
        }
        return ClientRequest{ClientProtocol::LengthPrefixed,
                             nlohmann::json::parse(payload)};
    }

    bool send_response(int fd, ClientProtocol protocol, const nlohmann::json& response) {
        std::string payload = response.dump();

        if (protocol == ClientProtocol::NewlineJson) {
            payload.push_back('\n');
            return send_all(fd, payload.data(), payload.size());
        }

        uint32_t len = static_cast<uint32_t>(payload.size());
        return send_all(fd, reinterpret_cast<const char*>(&len), sizeof(len)) &&
               send_all(fd, payload.data(), payload.size());
    }

    nlohmann::json dispatch(const nlohmann::json& request) {
        auto id = request.value("id", nlohmann::json(nullptr));
        std::string method = request.value("method", request.value("cmd", ""));

        if (method == "status") {
            return result_response(id, snapshot_to_json(scorer_.latest()));
        }
        if (method == "history") {
            int limit = 60;
            if (request.contains("params") && request["params"].is_object()) {
                limit = request["params"].value("limit", limit);
            }
            limit = std::clamp(limit, 1, 1440);

            nlohmann::json history = nlohmann::json::array();
            for (const auto& snap : scorer_.history(limit)) {
                history.push_back(snapshot_to_json(snap));
            }
            return result_response(id, history);
        }
        if (method == "report") {
            return result_response(id, scorer_.generate_html_report());
        }
        if (method == "check") {
            std::string name;
            if (request.contains("params") && request["params"].is_object()) {
                name = request["params"].value("name", "");
            }

            auto checks = HealthChecks::run_all();
            nlohmann::json result = nlohmann::json::array();
            for (const auto& check : checks) {
                if (name.empty() || check.name == name) {
                    result.push_back(check_to_json(check));
                }
            }
            return result_response(id, result);
        }

        return error_response(id, "unknown method: " + method);
    }

    static nlohmann::json result_response(const nlohmann::json& id,
                                          const nlohmann::json& result) {
        return {
            {"jsonrpc", "2.0"},
            {"id", id},
            {"result", result},
        };
    }

    static nlohmann::json error_response(const nlohmann::json& id,
                                         const std::string& message) {
        return {
            {"jsonrpc", "2.0"},
            {"id", id},
            {"error", {{"code", -32603}, {"message", message}}},
        };
    }
};

} // namespace straylight

int main(int /*argc*/, char* /*argv*/[]) {
    straylight::Log::init("straylight-health");

    auto cfg_result = straylight::Config::load("/etc/straylight/health.conf");
    if (!cfg_result.has_value()) {
        SL_ERROR("health: failed to load config: {}", cfg_result.error());
        return 1;
    }

    straylight::HealthDaemon daemon;
    return daemon.run(cfg_result.value());
}
