// lib/common/src/ipc.cpp
#include <straylight/ipc.h>
#include <straylight/log.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>
#include <cstring>
#include <vector>

// MSG_NOSIGNAL is Linux-only. This code targets Linux x86_64.
// Guard exists so editors/linters don't flag it on non-Linux dev tools.
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace straylight {

// --- IpcConnection ---

IpcConnection::IpcConnection(int fd) : fd_(fd) {}

IpcConnection::~IpcConnection() {
    if (fd_ >= 0) ::close(fd_);
}

IpcConnection::IpcConnection(IpcConnection&& o) noexcept : fd_(o.fd_) {
    o.fd_ = -1;
}

IpcConnection& IpcConnection::operator=(IpcConnection&& o) noexcept {
    if (this != &o) {
        if (fd_ >= 0) ::close(fd_);
        fd_ = o.fd_;
        o.fd_ = -1;
    }
    return *this;
}

int IpcConnection::fd() const noexcept { return fd_; }

Result<void, std::string> IpcConnection::send(std::string_view message) {
    if (message.size() > 16u * 1024 * 1024) {
        return Result<void, std::string>::error("Message exceeds 16MB limit");
    }
    uint32_t len = static_cast<uint32_t>(message.size());
    // Send length prefix (4 bytes, network order not needed for local IPC)
    if (::send(fd_, &len, sizeof(len), MSG_NOSIGNAL) != sizeof(len)) {
        return Result<void, std::string>::error("Failed to send message length");
    }
    size_t sent = 0;
    while (sent < message.size()) {
        auto n = ::send(fd_, message.data() + sent, message.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            return Result<void, std::string>::error("Failed to send message body");
        }
        sent += static_cast<size_t>(n);
    }
    return Result<void, std::string>::ok();
}

Result<std::string, std::string> IpcConnection::receive() {
    uint32_t len = 0;
    auto n = ::recv(fd_, &len, sizeof(len), MSG_WAITALL);
    if (n != sizeof(len)) {
        return Result<std::string, std::string>::error("Failed to receive message length");
    }
    if (len > 16 * 1024 * 1024) {  // 16MB max message
        return Result<std::string, std::string>::error("Message too large");
    }
    std::string buf(len, '\0');
    size_t received = 0;
    while (received < len) {
        auto r = ::recv(fd_, buf.data() + received, len - received, 0);
        if (r <= 0) {
            return Result<std::string, std::string>::error("Failed to receive message body");
        }
        received += static_cast<size_t>(r);
    }
    return Result<std::string, std::string>::ok(std::move(buf));
}

// --- IpcServer ---

IpcServer::IpcServer() = default;

IpcServer::~IpcServer() {
    if (fd_ >= 0) {
        ::close(fd_);
        if (!path_.empty()) ::unlink(path_.c_str());
    }
}

Result<void, std::string> IpcServer::bind(const std::string& path) {
    if (fd_ >= 0) {
        ::close(fd_);
        if (!path_.empty()) ::unlink(path_.c_str());
        fd_ = -1;
        path_.clear();
    }
    fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ < 0) {
        return Result<void, std::string>::error("socket() failed");
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    ::unlink(path.c_str());

    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        return Result<void, std::string>::error("bind() failed: " + std::string(strerror(errno)));
    }

    if (::listen(fd_, 16) < 0) {
        return Result<void, std::string>::error("listen() failed");
    }

    path_ = path;
    SL_DEBUG("IPC server listening on {}", path);
    return Result<void, std::string>::ok();
}

Result<std::unique_ptr<IpcConnection>, std::string> IpcServer::accept(int timeout_ms) {
    if (timeout_ms > 0) {
        pollfd pfd{fd_, POLLIN, 0};
        int ret = ::poll(&pfd, 1, timeout_ms);
        if (ret == 0) {
            return Result<std::unique_ptr<IpcConnection>, std::string>::error("accept() timed out");
        }
        if (ret < 0) {
            return Result<std::unique_ptr<IpcConnection>, std::string>::error("poll() failed");
        }
    }

    int client_fd = ::accept(fd_, nullptr, nullptr);
    if (client_fd < 0) {
        return Result<std::unique_ptr<IpcConnection>, std::string>::error("accept() failed");
    }

    return Result<std::unique_ptr<IpcConnection>, std::string>::ok(
        std::unique_ptr<IpcConnection>(new IpcConnection(client_fd)));
}

// --- IpcClient ---

IpcClient::IpcClient() : IpcConnection(-1) {}

Result<void, std::string> IpcClient::connect(const std::string& path) {
    if (fd_ >= 0) ::close(fd_);
    fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ < 0) {
        return Result<void, std::string>::error("socket() failed");
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        return Result<void, std::string>::error("connect() failed: " + std::string(strerror(errno)));
    }

    SL_DEBUG("IPC client connected to {}", path);
    return Result<void, std::string>::ok();
}

} // namespace straylight
