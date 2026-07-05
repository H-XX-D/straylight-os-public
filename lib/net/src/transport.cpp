#include <straylight/net/transport.h>
#include <cstring>

namespace straylight::net {

straylight::Result<void, std::string> send_tensor(
    UdpSocket& sock,
    const std::string& dest_addr, uint16_t dest_port,
    const TensorDesc& desc,
    const void* data) {

    TensorHeader hdr{};
    hdr.dtype = static_cast<uint8_t>(desc.dtype);
    hdr.ndim = static_cast<uint8_t>(desc.shape.size());
    hdr.data_size = desc.nbytes();
    for (size_t i = 0; i < desc.shape.size() && i < 8; i++) {
        hdr.shape[i] = desc.shape[i];
    }

    auto r1 = sock.send_to(dest_addr, dest_port, &hdr, sizeof(hdr));
    if (!r1.has_value()) return straylight::Result<void, std::string>::error(r1.error());

    if (data && hdr.data_size > 0) {
        auto r2 = sock.send_to(dest_addr, dest_port, data, hdr.data_size);
        if (!r2.has_value()) return straylight::Result<void, std::string>::error(r2.error());
    }

    return straylight::Result<void, std::string>::ok();
}

straylight::Result<TensorHeader, std::string> recv_tensor_header(
    UdpSocket& sock, int timeout_ms) {
    TensorHeader hdr{};
    auto r = sock.recv_from(&hdr, sizeof(hdr), timeout_ms);
    if (!r.has_value()) {
        return straylight::Result<TensorHeader, std::string>::error(r.error());
    }
    if (r.value().bytes_received != sizeof(TensorHeader)) {
        return straylight::Result<TensorHeader, std::string>::error(
            "Truncated tensor header: got " + std::to_string(r.value().bytes_received) +
            " bytes, expected " + std::to_string(sizeof(TensorHeader)));
    }
    if (hdr.magic != 0x53544C54) {
        return straylight::Result<TensorHeader, std::string>::error("Invalid tensor header magic");
    }
    return straylight::Result<TensorHeader, std::string>::ok(hdr);
}

} // namespace straylight::net
