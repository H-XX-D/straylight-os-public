#pragma once

#include <straylight/export.h>
#include <straylight/result.h>
#include <straylight/net/socket.h>
#include <straylight/net/protocol.h>
#include <straylight/types.h>

#include <string>

namespace straylight::net {

/// Send a tensor descriptor + data over UDP.
STRAYLIGHT_EXPORT
straylight::Result<void, std::string> send_tensor(
    UdpSocket& sock,
    const std::string& dest_addr, uint16_t dest_port,
    const TensorDesc& desc,
    const void* data);

/// Receive a tensor header. Caller must then recv the data payload.
STRAYLIGHT_EXPORT
straylight::Result<TensorHeader, std::string> recv_tensor_header(
    UdpSocket& sock,
    int timeout_ms = 0);

} // namespace straylight::net
