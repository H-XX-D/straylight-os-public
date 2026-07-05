// bin/bus/bus_daemon.cpp
#include "bus_daemon.h"

#include <straylight/log.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <sstream>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace straylight {

// ─── helpers ────────────────────────────────────────────────────────────────

static bool set_nonblocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

// ─── DaemonBase overrides ───────────────────────────────────────────────────

Result<void, SLError> BusDaemon::init(const Config& cfg) {
    socket_path_ = cfg.get<std::string>("bus.socket", socket_path_);

    auto res = setup_ipc();
    if (!res.has_value()) return res;

    SL_INFO("bus: initialized, listening on {}", socket_path_);
    return Result<void, SLError>::ok();
}

Result<void, SLError> BusDaemon::tick() {
    // Drive the epoll loop — handle accepts + client messages
    poll_ipc(80);

    // Emit periodic state event to connected clients
    {
        std::lock_guard lock(mutex_);
        if (!client_fds_.empty()) {
            nlohmann::json state;
            state["event"] = "state";
            nlohmann::json svcs = nlohmann::json::array();
            for (auto& [name, pid] : service_registry_) {
                svcs.push_back({{"name", name}, {"pid", pid},
                                {"registered_at_ms", 0}});
            }
            state["payload"]["services"] = svcs;
            state["payload"]["service_count"] = service_registry_.size();
            state["payload"]["messages_routed"] = messages_routed_;
            auto msg = state.dump();
            for (int fd : client_fds_) send_frame(fd, msg);
        }
    }

    return Result<void, SLError>::ok();
}

void BusDaemon::shutdown() {
    SL_INFO("bus: shutting down");
    teardown_ipc();
}

// ─── service registry ────────────────────────────────────────────────────────

Result<void, SLError> BusDaemon::register_service(const std::string& name, pid_t owner) {
    std::lock_guard lock(mutex_);
    if (service_registry_.contains(name))
        return Result<void, SLError>::error(
            SLError{SLErrorCode::AlreadyExists, "service already registered: " + name});
    service_registry_[name] = owner;
    SL_DEBUG("bus: registered service {} (pid={})", name, owner);
    return Result<void, SLError>::ok();
}

void BusDaemon::unregister_service(const std::string& name) {
    std::lock_guard lock(mutex_);
    service_registry_.erase(name);
    subscriptions_.erase(name);
}

std::optional<pid_t> BusDaemon::lookup_owner(const std::string& name) const {
    std::lock_guard lock(mutex_);
    auto it = service_registry_.find(name);
    if (it == service_registry_.end()) return std::nullopt;
    return it->second;
}

// ─── signal forwarding ───────────────────────────────────────────────────────

void BusDaemon::subscribe(const std::string& service, const std::string& signal,
                           SignalHandler handler) {
    std::lock_guard lock(mutex_);
    subscriptions_[service + "." + signal].push_back(std::move(handler));
}

void BusDaemon::emit(const std::string& service, const std::string& signal,
                     const std::string& payload) {
    std::vector<SignalHandler> handlers;
    {
        std::lock_guard lock(mutex_);
        auto key = service + "." + signal;
        if (auto it = subscriptions_.find(key); it != subscriptions_.end())
            handlers = it->second;
        ++messages_routed_;
    }
    for (auto& h : handlers) h(payload);

    // Fan out to UI bridge clients as a JSON event
    nlohmann::json env;
    env["service"] = service;
    env["signal"] = signal;
    env["payload"] = payload;
    auto msg = env.dump();
    std::lock_guard lock(mutex_);
    for (int fd : client_fds_) send_frame(fd, msg);
}

// ─── IPC socket setup ────────────────────────────────────────────────────────

Result<void, SLError> BusDaemon::setup_ipc() {
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(socket_path_).parent_path(), ec);

    ::unlink(socket_path_.c_str());

    server_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd_ < 0)
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError,
                    std::string("socket(): ") + std::strerror(errno)});

    set_nonblocking(server_fd_);

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);
    if (::bind(server_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(server_fd_);
        server_fd_ = -1;
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError,
                    std::string("bind(): ") + std::strerror(errno)});
    }
    ::listen(server_fd_, SOMAXCONN);

    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        ::close(server_fd_);
        server_fd_ = -1;
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError,
                    std::string("epoll_create1(): ") + std::strerror(errno)});
    }

    struct epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = server_fd_;
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, server_fd_, &ev);

    return Result<void, SLError>::ok();
}

void BusDaemon::teardown_ipc() {
    for (int fd : client_fds_) {
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
        ::close(fd);
    }
    client_fds_.clear();
    if (epoll_fd_ >= 0) { ::close(epoll_fd_); epoll_fd_ = -1; }
    if (server_fd_ >= 0) { ::close(server_fd_); server_fd_ = -1; }
    ::unlink(socket_path_.c_str());
}

void BusDaemon::poll_ipc(int timeout_ms) {
    if (epoll_fd_ < 0) return;
    constexpr int MAX_EVENTS = 32;
    struct epoll_event events[MAX_EVENTS];
    int n = ::epoll_wait(epoll_fd_, events, MAX_EVENTS, timeout_ms);
    for (int i = 0; i < n; ++i) {
        if (events[i].data.fd == server_fd_) {
            handle_accept();
        } else {
            handle_client(events[i].data.fd, events[i].events);
        }
    }
}

void BusDaemon::handle_accept() {
    while (true) {
        struct sockaddr_un addr{};
        socklen_t len = sizeof(addr);
        int fd = ::accept(server_fd_, reinterpret_cast<struct sockaddr*>(&addr), &len);
        if (fd < 0) break;
        set_nonblocking(fd);
        struct epoll_event ev{};
        ev.events = EPOLLIN | EPOLLHUP;
        ev.data.fd = fd;
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev);
        std::lock_guard lock(mutex_);
        client_fds_.push_back(fd);
        SL_DEBUG("bus: client connected fd={}", fd);
    }
}

void BusDaemon::handle_client(int fd, uint32_t events) {
    if (events & (EPOLLHUP | EPOLLERR)) { remove_client(fd); return; }
    auto res = recv_frame(fd);
    if (!res.has_value()) { remove_client(fd); return; }
    auto response = dispatch(res.value());
    send_frame(fd, response);
}

void BusDaemon::remove_client(int fd) {
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    ::close(fd);
    std::lock_guard lock(mutex_);
    client_fds_.erase(std::remove(client_fds_.begin(), client_fds_.end(), fd),
                      client_fds_.end());
}

// ─── framing ────────────────────────────────────────────────────────────────

Result<std::string, std::string> BusDaemon::recv_frame(int fd) {
    uint32_t len = 0;
    if (::recv(fd, &len, 4, MSG_WAITALL) != 4)
        return Result<std::string, std::string>::error("short read on length");
    if (len > 4u * 1024 * 1024)
        return Result<std::string, std::string>::error("frame too large");
    std::string buf(len, '\0');
    size_t got = 0;
    while (got < len) {
        auto r = ::recv(fd, buf.data() + got, len - got, 0);
        if (r <= 0) return Result<std::string, std::string>::error("read error");
        got += static_cast<size_t>(r);
    }
    return Result<std::string, std::string>::ok(std::move(buf));
}

void BusDaemon::send_frame(int fd, const std::string& msg) {
    uint32_t len = static_cast<uint32_t>(msg.size());
    ::send(fd, &len, 4, MSG_NOSIGNAL);
    ::send(fd, msg.data(), len, MSG_NOSIGNAL);
}

// ─── JSON-RPC dispatch ───────────────────────────────────────────────────────

std::string BusDaemon::dispatch(const std::string& json_req) {
    nlohmann::json req, res;
    try { req = nlohmann::json::parse(json_req); }
    catch (...) { return R"({"error":"invalid json"})"; }

    const auto id = req.value("id", "");
    const auto method = req.value("method", "");
    res["id"] = id;

    std::lock_guard lock(mutex_);

    if (method == "list_services") {
        nlohmann::json svcs = nlohmann::json::array();
        for (auto& [name, pid] : service_registry_)
            svcs.push_back({{"name", name}, {"pid", pid}});
        res["result"]["services"] = svcs;
        res["result"]["count"] = service_registry_.size();
        res["result"]["messages_routed"] = messages_routed_;
    } else if (method == "lookup") {
        const auto name = req["params"].value("name", "");
        auto it = service_registry_.find(name);
        if (it != service_registry_.end())
            res["result"] = {{"pid", it->second}};
        else
            res["error"] = "not found";
    } else if (method == "status") {
        res["result"]["service_count"] = service_registry_.size();
        res["result"]["messages_routed"] = messages_routed_;
        res["result"]["client_count"] = client_fds_.size();
    } else {
        res["error"] = "unknown method: " + method;
    }

    return res.dump();
}

} // namespace straylight
