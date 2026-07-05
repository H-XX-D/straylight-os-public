// bin/core/core_daemon.cpp
#include "core_daemon.h"
#include <thread>
#include <chrono>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#include <fcntl.h>
#include <grp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace straylight {

static bool core_set_nonblocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static std::string core_trim(std::string value) {
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r' ||
                             value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    size_t start = 0;
    while (start < value.size() &&
           (value[start] == '\n' || value[start] == '\r' ||
            value[start] == ' ' || value[start] == '\t')) {
        ++start;
    }
    if (start > 0) value.erase(0, start);
    return value;
}

static std::string core_run_cmd(const std::string& cmd) {
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) return "";
    std::string result;
    char buf[256];
    while (::fgets(buf, sizeof(buf), pipe)) {
        result += buf;
    }
    ::pclose(pipe);
    return result;
}

static HealthStatus core_systemd_health(const std::string& unit) {
    const auto status =
        core_trim(core_run_cmd("systemctl is-active " + unit + ".service 2>/dev/null"));
    if (status == "active") return HealthStatus::Healthy;
    if (status == "activating" || status == "reloading" || status == "deactivating") {
        return HealthStatus::Degraded;
    }
    if (status.empty() || status == "unknown") return HealthStatus::Unknown;
    return HealthStatus::Failed;
}

Result<void, SLError> CoreDaemon::init(const Config& cfg) {
    poll_interval_s_ = cfg.get<int>("core.poll_interval_s", 10);
    restart_max_ = cfg.get<int>("core.restart_max", 5);
    socket_path_ = cfg.get<std::string>("core.socket", socket_path_);

    // Register known subsystems in boot order
    register_subsystem("straylight-entropy",   SubsystemPriority::Critical);
    register_subsystem("straylight-bus",       SubsystemPriority::Critical);
    register_subsystem("straylight-registry",  SubsystemPriority::Critical);
    register_subsystem("straylight-scheduler", SubsystemPriority::Normal);

    SL_INFO("core: initialized with {} subsystems ({} critical)",
            pipeline_.subsystem_count(), pipeline_.critical_count());

    auto res = setup_ipc();
    if (!res.has_value())
        SL_WARN("core: IPC socket not available: {}", res.error().message());

    return Result<void, SLError>::ok();
}

Result<void, SLError> CoreDaemon::tick() {
    // Poll IPC events before subsystem work, then keep servicing IPC during
    // the configured interval so the Unix socket backlog cannot wedge.
    poll_ipc(50);

    // Poll subsystem health
    for (auto& entry : pipeline_.subsystems()) {
        const auto status = core_systemd_health(entry.name);
        doctor_.record_health(entry.name, status);
        entry.last_health = status;
        SL_DEBUG("core: {} systemd health={}", entry.name,
                 status == HealthStatus::Healthy ? "healthy" :
                 status == HealthStatus::Degraded ? "degraded" :
                 status == HealthStatus::Failed ? "failed" : "unknown");

        if (doctor_.needs_restart(entry.name)) {
            auto it = restart_counts_.find(entry.name);
            int count = (it != restart_counts_.end()) ? it->second : 0;

            if (count < restart_max_) {
                SL_WARN("core: restarting {} (attempt {}/{})",
                        entry.name, count + 1, restart_max_);
                restart_counts_[entry.name] = count + 1;
            } else {
                SL_ERROR("core: {} exceeded max restarts ({}), marking failed",
                         entry.name, restart_max_);
            }
        }
    }

    check_readiness();

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(poll_interval_s_);
    while (!shutdown_requested() && std::chrono::steady_clock::now() < deadline) {
        poll_ipc(100);
    }
    return Result<void, SLError>::ok();
}

void CoreDaemon::shutdown() {
    SL_INFO("core: shutting down orchestrator");
    teardown_ipc();
}

void CoreDaemon::register_subsystem(const std::string& name, SubsystemPriority prio) {
    std::lock_guard lock(mutex_);
    pipeline_.register_subsystem(name, prio);
}

void CoreDaemon::on_health_update(const std::string& name, HealthStatus status) {
    std::lock_guard lock(mutex_);
    doctor_.record_health(name, status);

    for (auto& entry : pipeline_.subsystems()) {
        if (entry.name == name) {
            entry.last_health = status;
            break;
        }
    }

    check_readiness();
}

bool CoreDaemon::is_ready() const {
    std::lock_guard lock(mutex_);
    return ready_;
}

void CoreDaemon::check_readiness() {
    for (const auto& entry : pipeline_.subsystems()) {
        if (entry.priority == SubsystemPriority::Critical &&
            entry.last_health != HealthStatus::Healthy) {
            ready_ = false;
            return;
        }
    }
    if (!ready_) {
        SL_INFO("core: all critical subsystems healthy — system ready");
    }
    ready_ = true;
}

// ─── IPC socket ──────────────────────────────────────────────────────────────

Result<void, SLError> CoreDaemon::setup_ipc() {
    std::filesystem::create_directories("/run/straylight");

    server_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd_ < 0)
        return Result<void, SLError>::error(SLError{SLErrorCode::IOError, std::strerror(errno)});

    core_set_nonblocking(server_fd_);

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    ::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);
    ::unlink(socket_path_.c_str());

    if (::bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(server_fd_); server_fd_ = -1;
        return Result<void, SLError>::error(SLError{SLErrorCode::IOError, std::strerror(errno)});
    }
    ::listen(server_fd_, 8);

    if (auto* group = ::getgrnam("straylight")) {
        if (::chown(socket_path_.c_str(), 0, group->gr_gid) != 0) {
            auto err = std::string("core chown(): ") + std::strerror(errno);
            ::close(server_fd_);
            server_fd_ = -1;
            return Result<void, SLError>::error(SLError{SLErrorCode::IOError, err});
        }
    }
    ::chmod(socket_path_.c_str(), 0660);

    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        ::close(server_fd_); server_fd_ = -1;
        return Result<void, SLError>::error(SLError{SLErrorCode::IOError, std::strerror(errno)});
    }

    epoll_event ev{};
    ev.events  = EPOLLIN;
    ev.data.fd = server_fd_;
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, server_fd_, &ev);

    SL_INFO("core: IPC listening on {}", socket_path_);
    return Result<void, SLError>::ok();
}

void CoreDaemon::teardown_ipc() {
    for (int fd : client_fds_) ::close(fd);
    client_fds_.clear();
    if (epoll_fd_ >= 0) { ::close(epoll_fd_); epoll_fd_ = -1; }
    if (server_fd_ >= 0) { ::close(server_fd_); server_fd_ = -1; }
    ::unlink(socket_path_.c_str());
}

void CoreDaemon::poll_ipc(int timeout_ms) {
    if (epoll_fd_ < 0) return;

    epoll_event events[16];
    int n = ::epoll_wait(epoll_fd_, events, 16, timeout_ms);
    for (int i = 0; i < n; ++i) {
        if (events[i].data.fd == server_fd_)
            handle_accept();
        else
            handle_client(events[i].data.fd);
    }

    // Periodic fan-out: push state to all connected clients
    if (!client_fds_.empty()) {
        nlohmann::json ev;
        ev["service"] = "core";
        ev["type"]    = "event";
        ev["payload"] = build_state_payload();
        send_to_all(ev.dump());
    }
}

void CoreDaemon::handle_accept() {
    while (true) {
        sockaddr_un addr{};
        socklen_t len = sizeof(addr);
        int fd = ::accept4(
            server_fd_,
            reinterpret_cast<sockaddr*>(&addr),
            &len,
            SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                SL_WARN("core: accept failed: {}", std::strerror(errno));
            }
            return;
        }

        epoll_event ev{};
        ev.events  = EPOLLIN | EPOLLHUP | EPOLLERR;
        ev.data.fd = fd;
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev);
        client_fds_.push_back(fd);
        SL_DEBUG("core: IPC client connected (fd={})", fd);
    }
}

void CoreDaemon::handle_client(int fd) {
    auto frame = recv_frame(fd);
    if (!frame) { remove_client(fd); return; }

    try {
        auto msg = nlohmann::json::parse(*frame);
        auto reply = dispatch(msg);
        send_frame(fd, reply.dump());
    } catch (...) {
        nlohmann::json err;
        err["service"] = "core";
        err["type"]    = "error";
        err["payload"]["message"] = "parse error";
        send_frame(fd, err.dump());
    }
}

void CoreDaemon::remove_client(int fd) {
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    ::close(fd);
    client_fds_.erase(std::remove(client_fds_.begin(), client_fds_.end(), fd), client_fds_.end());
}

std::optional<std::string> CoreDaemon::recv_frame(int fd) {
    uint32_t len_le = 0;
    if (::recv(fd, &len_le, 4, MSG_WAITALL) != 4) return std::nullopt;
    uint32_t len = __builtin_bswap32(__builtin_bswap32(len_le)); // already LE on LE hosts
    if (len == 0 || len > 4 * 1024 * 1024) return std::nullopt;
    std::string buf(len, '\0');
    ssize_t got = ::recv(fd, buf.data(), len, MSG_WAITALL);
    if (got != static_cast<ssize_t>(len)) return std::nullopt;
    return buf;
}

bool CoreDaemon::send_frame(int fd, const std::string& data) {
    uint32_t len = static_cast<uint32_t>(data.size());
    if (::send(fd, &len, 4, MSG_NOSIGNAL) != 4) return false;
    return ::send(fd, data.data(), len, MSG_NOSIGNAL) == static_cast<ssize_t>(len);
}

void CoreDaemon::send_to_all(const std::string& data) {
    std::vector<int> dead;
    for (int fd : client_fds_)
        if (!send_frame(fd, data)) dead.push_back(fd);
    for (int fd : dead) remove_client(fd);
}

nlohmann::json CoreDaemon::build_state_payload() {
    std::lock_guard lock(mutex_);
    nlohmann::json p;
    p["ready"] = ready_;
    p["subsystems"] = nlohmann::json::array();
    int crit = 0, healthy = 0, failed = 0;
    for (const auto& e : pipeline_.subsystems()) {
        nlohmann::json s;
        s["name"]          = e.name;
        s["priority"]      = (e.priority == SubsystemPriority::Critical) ? "critical" : "normal";
        s["last_health"]   = (e.last_health == HealthStatus::Healthy) ? "healthy" :
                             (e.last_health == HealthStatus::Degraded) ? "degraded" : "unknown";
        s["restart_count"] = restart_counts_.count(e.name) ? restart_counts_.at(e.name) : 0;
        if (e.priority == SubsystemPriority::Critical) ++crit;
        if (e.last_health == HealthStatus::Healthy) ++healthy;
        else ++failed;
        p["subsystems"].push_back(s);
    }
    p["critical_count"] = crit;
    p["healthy_count"]  = healthy;
    p["failed_count"]   = failed;
    return p;
}

nlohmann::json CoreDaemon::dispatch(const nlohmann::json& msg) {
    nlohmann::json r;
    r["service"] = "core";
    r["type"]    = "res";
    if (msg.contains("id")) r["id"] = msg["id"];

    std::string method = msg.value("method", "");

    if (method == "status") {
        r["payload"] = build_state_payload();
    } else if (method == "readiness") {
        r["payload"]["ready"] = ready_;
    } else if (method == "restart") {
        std::string sub = msg.value("subsystem", "");
        if (sub.empty()) {
            r["type"] = "error";
            r["payload"]["message"] = "subsystem required";
        } else {
            std::string cmd = "systemctl restart " + sub;
            int rc = ::system(cmd.c_str());
            r["payload"]["ok"]   = (rc == 0);
            r["payload"]["name"] = sub;
        }
    } else {
        r["type"] = "error";
        r["payload"]["message"] = "unknown method: " + method;
    }

    return r;
}

} // namespace straylight
