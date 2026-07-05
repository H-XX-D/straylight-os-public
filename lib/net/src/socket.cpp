#include <straylight/net/socket.h>
#include <straylight/log.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

namespace straylight::net {

UdpSocket::UdpSocket() = default;

UdpSocket::~UdpSocket() {
    if (fd_ >= 0) ::close(fd_);
}

UdpSocket::UdpSocket(UdpSocket&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }

UdpSocket& UdpSocket::operator=(UdpSocket&& o) noexcept {
    if (this != &o) {
        if (fd_ >= 0) ::close(fd_);
        fd_ = o.fd_;
        o.fd_ = -1;
    }
    return *this;
}

straylight::Result<void, std::string> UdpSocket::bind(const std::string& addr, uint16_t port) {
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) return straylight::Result<void, std::string>::error("socket() failed");

    int opt = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    inet_pton(AF_INET, addr.c_str(), &sa.sin_addr);

    if (::bind(fd_, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) < 0) {
        return straylight::Result<void, std::string>::error(
            std::string("bind() failed: ") + strerror(errno));
    }

    return straylight::Result<void, std::string>::ok();
}

straylight::Result<size_t, std::string> UdpSocket::send_to(
    const std::string& addr, uint16_t port, const void* data, size_t len) {
    if (fd_ < 0) {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) return straylight::Result<size_t, std::string>::error("socket() failed");
    }

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    inet_pton(AF_INET, addr.c_str(), &sa.sin_addr);

    auto sent = ::sendto(fd_, data, len, 0, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
    if (sent < 0) {
        return straylight::Result<size_t, std::string>::error("sendto() failed");
    }
    return straylight::Result<size_t, std::string>::ok(static_cast<size_t>(sent));
}

straylight::Result<RecvResult, std::string> UdpSocket::recv_from(
    void* buf, size_t buf_len, int timeout_ms) {
    if (timeout_ms > 0) {
        pollfd pfd{fd_, POLLIN, 0};
        int ret = ::poll(&pfd, 1, timeout_ms);
        if (ret == 0) return straylight::Result<RecvResult, std::string>::error("recv timeout");
        if (ret < 0) return straylight::Result<RecvResult, std::string>::error("poll() failed");
    }

    sockaddr_in sa{};
    socklen_t sa_len = sizeof(sa);
    auto n = ::recvfrom(fd_, buf, buf_len, 0, reinterpret_cast<sockaddr*>(&sa), &sa_len);
    if (n < 0) {
        return straylight::Result<RecvResult, std::string>::error("recvfrom() failed");
    }

    char addr_buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &sa.sin_addr, addr_buf, sizeof(addr_buf));

    return straylight::Result<RecvResult, std::string>::ok(
        RecvResult{static_cast<size_t>(n), addr_buf, ntohs(sa.sin_port)});
}

uint16_t UdpSocket::local_port() const {
    sockaddr_in sa{};
    socklen_t len = sizeof(sa);
    getsockname(fd_, reinterpret_cast<sockaddr*>(&sa), &len);
    return ntohs(sa.sin_port);
}

} // namespace straylight::net
