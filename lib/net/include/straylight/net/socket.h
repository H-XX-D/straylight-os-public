#pragma once

#include <straylight/export.h>
#include <straylight/result.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace straylight::net {

struct RecvResult {
    size_t bytes_received;
    std::string sender_addr;
    uint16_t sender_port;
};

/// UDP socket for datagram-based tensor transport.
class STRAYLIGHT_EXPORT UdpSocket {
public:
    UdpSocket();
    ~UdpSocket();

    UdpSocket(UdpSocket&&) noexcept;
    UdpSocket& operator=(UdpSocket&&) noexcept;

    /// Bind to address and port (port 0 = OS picks ephemeral).
    straylight::Result<void, std::string> bind(const std::string& addr, uint16_t port);

    /// Send data to a specific address.
    straylight::Result<size_t, std::string> send_to(
        const std::string& addr, uint16_t port,
        const void* data, size_t len);

    /// Receive data. timeout_ms=0 blocks forever.
    straylight::Result<RecvResult, std::string> recv_from(
        void* buf, size_t buf_len, int timeout_ms = 0);

    /// Get the port we're bound to.
    [[nodiscard]] uint16_t local_port() const;

    [[nodiscard]] int fd() const noexcept { return fd_; }

private:
    int fd_ = -1;
};

} // namespace straylight::net
