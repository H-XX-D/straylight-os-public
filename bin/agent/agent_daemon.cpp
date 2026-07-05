// bin/agent/agent_daemon.cpp
#include "agent_daemon.h"

#include <straylight/log.h>

#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>

// MSG_NOSIGNAL is Linux-only; guard for cross-platform editors.
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace straylight {

static constexpr const char* kDefaultSocketPath = "/run/straylight/agent.sock";
static constexpr size_t kMaxBatchPerTick = 10;

/// Helper: set a file descriptor to non-blocking mode.
static bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

// ---------------------------------------------------------------------------
// init
// ---------------------------------------------------------------------------

Result<void, SLError> AgentDaemon::init(const Config& cfg) {
    SL_INFO("agent: initializing");

    socket_path_ = cfg.get<std::string>("agent.socket", kDefaultSocketPath);

    // Ensure parent directory exists
    {
        std::error_code ec;
        std::filesystem::create_directories(
            std::filesystem::path(socket_path_).parent_path(), ec);
        if (ec) {
            SL_WARN("agent: cannot create socket directory: {}", ec.message());
        }
    }

    // Remove stale socket if present
    ::unlink(socket_path_.c_str());

    // Create Unix domain socket
    server_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError,
                    std::string("socket() failed: ") + std::strerror(errno)});
    }

    if (!set_nonblocking(server_fd_)) {
        ::close(server_fd_);
        server_fd_ = -1;
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError, "failed to set server socket non-blocking"});
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(server_fd_, reinterpret_cast<struct sockaddr*>(&addr),
               sizeof(addr)) < 0) {
        ::close(server_fd_);
        server_fd_ = -1;
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError,
                    std::string("bind() failed on ") + socket_path_ + ": " +
                        std::strerror(errno)});
    }

    if (::listen(server_fd_, SOMAXCONN) < 0) {
        ::close(server_fd_);
        server_fd_ = -1;
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError,
                    std::string("listen() failed: ") + std::strerror(errno)});
    }

    // Register server socket with the event loop for incoming connections
    auto add_res = loop_.add_fd(server_fd_, EPOLLIN,
                                [this](uint32_t ev) { handle_accept(ev); });
    if (!add_res.has_value()) {
        ::close(server_fd_);
        server_fd_ = -1;
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError,
                    std::string("event loop add_fd failed: ") + add_res.error()});
    }

    SL_INFO("agent: listening on {}", socket_path_);
    SL_INFO("agent: initialization complete");
    return Result<void, SLError>::ok();
}

// ---------------------------------------------------------------------------
// tick
// ---------------------------------------------------------------------------

Result<void, SLError> AgentDaemon::tick() {
    // Poll epoll for IPC events (accept new connections, receive submissions)
    auto res = loop_.run_once(100);
    if (!res.has_value()) {
        SL_WARN("agent: event loop error: {}", res.error());
    }

    // Process queued tasks
    drain_queue(kMaxBatchPerTick);

    return Result<void, SLError>::ok();
}

// ---------------------------------------------------------------------------
// shutdown
// ---------------------------------------------------------------------------

void AgentDaemon::shutdown() {
    SL_INFO("agent: shutting down");

    loop_.stop();

    // Close all client connections
    for (int fd : client_fds_) {
        loop_.remove_fd(fd);
        ::close(fd);
    }
    client_fds_.clear();

    // Close server socket
    if (server_fd_ >= 0) {
        loop_.remove_fd(server_fd_);
        ::close(server_fd_);
        server_fd_ = -1;
    }

    // Remove socket file
    ::unlink(socket_path_.c_str());

    SL_INFO("agent: shutdown complete");
}

// ---------------------------------------------------------------------------
// handle_accept
// ---------------------------------------------------------------------------

void AgentDaemon::handle_accept(uint32_t /*events*/) {
    // Accept all pending connections (level-triggered, drain loop)
    while (true) {
        struct sockaddr_un client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = ::accept(server_fd_,
                                 reinterpret_cast<struct sockaddr*>(&client_addr),
                                 &client_len);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;  // No more pending connections
            }
            SL_WARN("agent: accept() failed: {}", std::strerror(errno));
            break;
        }

        if (!set_nonblocking(client_fd)) {
            SL_WARN("agent: failed to set client fd non-blocking");
            ::close(client_fd);
            continue;
        }

        SL_DEBUG("agent: accepted client connection fd={}", client_fd);

        // Register client fd with event loop
        int cfd = client_fd;
        auto add_res = loop_.add_fd(client_fd, EPOLLIN | EPOLLHUP,
                                    [this, cfd](uint32_t ev) {
                                        handle_client(cfd, ev);
                                    });
        if (!add_res.has_value()) {
            SL_WARN("agent: failed to add client fd to event loop: {}",
                    add_res.error());
            ::close(client_fd);
            continue;
        }

        client_fds_.push_back(client_fd);
    }
}

// ---------------------------------------------------------------------------
// handle_client
// ---------------------------------------------------------------------------

void AgentDaemon::handle_client(int fd, uint32_t events) {
    if (events & (EPOLLHUP | EPOLLERR)) {
        SL_DEBUG("agent: client disconnected fd={}", fd);
        remove_client(fd);
        return;
    }

    if (events & EPOLLIN) {
        auto recv_res = recv_message(fd);
        if (!recv_res.has_value()) {
            SL_DEBUG("agent: client read error fd={}: {}", fd, recv_res.error());
            remove_client(fd);
            return;
        }

        auto proc_res = process_submission(recv_res.value());
        if (!proc_res.has_value()) {
            SL_WARN("agent: failed to process submission: {}", proc_res.error());
            send_message(fd, "ERR:" + proc_res.error());
        } else {
            send_message(fd, "OK");
        }
    }
}

// ---------------------------------------------------------------------------
// remove_client
// ---------------------------------------------------------------------------

void AgentDaemon::remove_client(int fd) {
    loop_.remove_fd(fd);
    ::close(fd);
    client_fds_.erase(
        std::remove(client_fds_.begin(), client_fds_.end(), fd),
        client_fds_.end());
}

// ---------------------------------------------------------------------------
// recv_message  (length-prefixed, compatible with IpcConnection protocol)
// ---------------------------------------------------------------------------

Result<std::string, std::string> AgentDaemon::recv_message(int fd) {
    uint32_t len = 0;
    auto n = ::recv(fd, &len, sizeof(len), MSG_WAITALL);
    if (n != static_cast<ssize_t>(sizeof(len))) {
        return Result<std::string, std::string>::error("failed to read message length");
    }
    if (len > 16u * 1024 * 1024) {
        return Result<std::string, std::string>::error("message too large");
    }

    std::string buf(len, '\0');
    size_t received = 0;
    while (received < len) {
        auto r = ::recv(fd, buf.data() + received, len - received, 0);
        if (r <= 0) {
            return Result<std::string, std::string>::error("failed to read message body");
        }
        received += static_cast<size_t>(r);
    }
    return Result<std::string, std::string>::ok(std::move(buf));
}

// ---------------------------------------------------------------------------
// send_message  (length-prefixed, compatible with IpcConnection protocol)
// ---------------------------------------------------------------------------

Result<void, std::string> AgentDaemon::send_message(int fd, std::string_view msg) {
    uint32_t len = static_cast<uint32_t>(msg.size());
    if (::send(fd, &len, sizeof(len), MSG_NOSIGNAL) != static_cast<ssize_t>(sizeof(len))) {
        return Result<void, std::string>::error("failed to send message length");
    }
    size_t sent = 0;
    while (sent < msg.size()) {
        auto n = ::send(fd, msg.data() + sent, msg.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            return Result<void, std::string>::error("failed to send message body");
        }
        sent += static_cast<size_t>(n);
    }
    return Result<void, std::string>::ok();
}

// ---------------------------------------------------------------------------
// process_submission
// ---------------------------------------------------------------------------

Result<void, std::string> AgentDaemon::process_submission(
    const std::string& message) {
    // Protocol: "TYPE:PRIORITY:PAYLOAD"
    // TYPE = inference|training|preprocess|custom
    // PRIORITY = low|normal|high|critical

    size_t first_colon = message.find(':');
    if (first_colon == std::string::npos) {
        return Result<void, std::string>::error("malformed submission: missing type");
    }
    size_t second_colon = message.find(':', first_colon + 1);
    if (second_colon == std::string::npos) {
        return Result<void, std::string>::error(
            "malformed submission: missing priority");
    }

    std::string type_str = message.substr(0, first_colon);
    std::string prio_str =
        message.substr(first_colon + 1, second_colon - first_colon - 1);
    std::string payload = message.substr(second_colon + 1);

    agent::TaskType type;
    if (type_str == "inference") {
        type = agent::TaskType::Inference;
    } else if (type_str == "training") {
        type = agent::TaskType::Training;
    } else if (type_str == "preprocess") {
        type = agent::TaskType::Preprocess;
    } else if (type_str == "custom") {
        type = agent::TaskType::Custom;
    } else {
        return Result<void, std::string>::error("unknown task type: " + type_str);
    }

    agent::Priority prio;
    if (prio_str == "low") {
        prio = agent::Priority::Low;
    } else if (prio_str == "normal") {
        prio = agent::Priority::Normal;
    } else if (prio_str == "high") {
        prio = agent::Priority::High;
    } else if (prio_str == "critical") {
        prio = agent::Priority::Critical;
    } else {
        return Result<void, std::string>::error("unknown priority: " + prio_str);
    }

    agent::Task task{};
    task.id = next_task_id_++;
    task.priority = prio;
    task.type = type;
    task.payload = std::move(payload);

    auto push_res = queue_.push(task);
    if (!push_res.has_value()) {
        return Result<void, std::string>::error(push_res.error());
    }

    SL_DEBUG("agent: enqueued task id={} type={} prio={}", task.id,
             static_cast<int>(task.type), static_cast<int>(task.priority));
    return Result<void, std::string>::ok();
}

// ---------------------------------------------------------------------------
// drain_queue
// ---------------------------------------------------------------------------

void AgentDaemon::drain_queue(size_t max_batch) {
    for (size_t i = 0; i < max_batch; ++i) {
        auto pop_res = queue_.pop();
        if (!pop_res.has_value()) {
            break;  // Queue empty
        }

        const auto& task = pop_res.value();

        const char* type_name = "unknown";
        switch (task.type) {
            case agent::TaskType::Inference:  type_name = "inference";  break;
            case agent::TaskType::Training:   type_name = "training";   break;
            case agent::TaskType::Preprocess: type_name = "preprocess"; break;
            case agent::TaskType::Custom:     type_name = "custom";     break;
        }

        SL_INFO("agent: executing task id={} type={} prio={} payload_size={}",
                task.id, type_name, static_cast<int>(task.priority),
                task.payload.size());

        // In later chunks this dispatches to the ML runtime (libstraylight-ml).
        // For now we log completion.
        SL_DEBUG("agent: task id={} completed", task.id);
    }
}

} // namespace straylight
