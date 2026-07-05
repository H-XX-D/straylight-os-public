/**
 * StrayLight Mirror Transfer — Implementation.
 *
 * Network transfer with compression, chunking, checksums,
 * and bandwidth throttling.
 */

#include "transfer.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace straylight::mirror {

// ---------------------------------------------------------------------------
// CRC32 (IEEE 802.3)
// ---------------------------------------------------------------------------

static const uint32_t crc32_table[256] = {
    0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f,
    0xe963a535, 0x9e6495a3, 0x0edb8832, 0x79dcb8a4, 0xe0d5e91b, 0x97d2d988,
    0x09b64c2b, 0x7eb17cbd, 0xe7b82d09, 0x90bf1d3d, 0x1db71064, 0x6ab020f2,
    0xf3b97148, 0x84be41de, 0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
    0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec, 0x14015c4f, 0x63066cd9,
    0xfa0f3d63, 0x8d080df5, 0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172,
    0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b, 0x35b5a8fa, 0x42b2986c,
    0xdbbbc9d6, 0xacbcf940, 0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
    0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f6b5, 0x56b3c423,
    0xcfba9599, 0xb8bda50f, 0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924,
    0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d, 0x76dc4190, 0x01db7106,
    0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
    0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dab, 0x086d3d2d,
    0x91646c97, 0xe6635c01, 0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e,
    0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457, 0x65b0d9c6, 0x12b7e950,
    0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
    0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7,
    0xa4d1c46d, 0xd3d6f4fb, 0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0,
    0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7822, 0x3b6e20c8, 0x4c69105e,
    0xd56041e4, 0xa2677172, 0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b,
    0x35b5a8fa, 0x42b2986c, 0xdbbbc9d6, 0xacbcf940, 0x32d86ce3, 0x45df5c75,
    0xdcd60dcf, 0xabd13d59, 0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116,
    0x21b4f6b5, 0x56b3c423, 0xcfba9599, 0xb8bda50f, 0x2802b89e, 0x5f058808,
    0xc60cd9b2, 0xb10be924, 0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d,
    0x76dc4190, 0x01db7106, 0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f,
    0x9fbfe4a5, 0xe8b8d433, 0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818,
    0x7f6a0dab, 0x086d3d2d, 0x91646c97, 0xe6635c01, 0x6b6b51f4, 0x1c6c6162,
    0x856530d8, 0xf262004e, 0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457,
    0x65b0d9c6, 0x12b7e950, 0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49,
    0x8cd37cf3, 0xfbd44c65, 0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2,
    0x4adfa541, 0x3dd895d7, 0xa4d1c46d, 0xd3d6f4fb, 0x4369e96a, 0x346ed9fc,
    0xad678846, 0xda60b8d0, 0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7822,
    0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f,
    0x72076785, 0x05005713, 0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38,
    0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21, 0x86d3d2d4, 0xf1d4e242,
    0x68ddb3f6, 0x1fda8360, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
    0x88085ae6, 0xff0f6b70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69,
    0x616bffd3, 0x166ccf45, 0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2,
    0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db, 0xaed16a4a, 0xd9d65adc,
    0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
    0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd706ff,
    0x54de5729, 0x23d967bf, 0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
    0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d
};

uint32_t Transfer::compute_crc32(const void* data, size_t len) {
    const uint8_t* buf = static_cast<const uint8_t*>(data);
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc = crc32_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

// ---------------------------------------------------------------------------
// Compression — Run-length + LZ77-lite implementation
// ---------------------------------------------------------------------------

std::vector<uint8_t> Transfer::compress(const std::vector<uint8_t>& data) {
    if (data.empty()) return {};

    std::vector<uint8_t> out;
    out.reserve(data.size());

    // Simple LZ77-style compression with 4KB sliding window.
    // Format: [literal_count:1][literals...] or [0xFF][offset:2][length:1]
    constexpr size_t kWindowSize = 4096;
    constexpr size_t kMinMatch = 4;
    constexpr size_t kMaxMatch = 255;

    size_t i = 0;
    std::vector<uint8_t> literal_buf;

    auto flush_literals = [&]() {
        while (!literal_buf.empty()) {
            size_t chunk = std::min(literal_buf.size(), size_t(254));
            out.push_back(static_cast<uint8_t>(chunk));
            out.insert(out.end(), literal_buf.begin(),
                       literal_buf.begin() + static_cast<ptrdiff_t>(chunk));
            literal_buf.erase(literal_buf.begin(),
                              literal_buf.begin() + static_cast<ptrdiff_t>(chunk));
        }
    };

    while (i < data.size()) {
        // Search for a match in the sliding window.
        size_t best_offset = 0;
        size_t best_length = 0;
        size_t window_start = (i > kWindowSize) ? (i - kWindowSize) : 0;

        for (size_t j = window_start; j < i; ++j) {
            size_t match_len = 0;
            while (match_len < kMaxMatch &&
                   i + match_len < data.size() &&
                   data[j + match_len] == data[i + match_len]) {
                ++match_len;
            }
            if (match_len >= kMinMatch && match_len > best_length) {
                best_offset = i - j;
                best_length = match_len;
            }
        }

        if (best_length >= kMinMatch) {
            flush_literals();
            out.push_back(0xFF); // Match marker
            out.push_back(static_cast<uint8_t>(best_offset & 0xFF));
            out.push_back(static_cast<uint8_t>((best_offset >> 8) & 0xFF));
            out.push_back(static_cast<uint8_t>(best_length));
            i += best_length;
        } else {
            literal_buf.push_back(data[i]);
            ++i;
        }
    }

    flush_literals();
    return out;
}

std::vector<uint8_t> Transfer::decompress(const std::vector<uint8_t>& data,
                                            uint32_t original_size) {
    std::vector<uint8_t> out;
    out.reserve(original_size);

    size_t i = 0;
    while (i < data.size() && out.size() < original_size) {
        uint8_t tag = data[i++];

        if (tag == 0xFF) {
            // Match: offset(2) + length(1)
            if (i + 2 >= data.size()) break;
            uint16_t offset = data[i] | (static_cast<uint16_t>(data[i + 1]) << 8);
            uint8_t length = data[i + 2];
            i += 3;

            if (offset == 0 || offset > out.size()) break;
            size_t src = out.size() - offset;
            for (uint8_t j = 0; j < length && out.size() < original_size; ++j) {
                out.push_back(out[src + j]);
            }
        } else {
            // Literal run of `tag` bytes.
            size_t count = tag;
            for (size_t j = 0; j < count && i < data.size() && out.size() < original_size; ++j) {
                out.push_back(data[i++]);
            }
        }
    }

    return out;
}

// ---------------------------------------------------------------------------
// Transfer implementation
// ---------------------------------------------------------------------------

Transfer::Transfer() {
    progress_.total_bytes = 0;
    progress_.transferred_bytes = 0;
    progress_.compressed_bytes = 0;
    progress_.compression_ratio = 1.0;
    progress_.throughput_mbps = 0.0;
    progress_.complete = false;
    progress_.start_time = std::chrono::steady_clock::now();
}

Transfer::~Transfer() {
    close();
}

VoidResult<std::string> Transfer::connect(const std::string& host, uint16_t port) {
    struct addrinfo hints{}, *result = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    std::string port_str = std::to_string(port);
    int ret = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result);
    if (ret != 0) {
        return VoidResult<std::string>::error(
            "getaddrinfo failed: " + std::string(gai_strerror(ret)));
    }

    socket_fd_ = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (socket_fd_ < 0) {
        freeaddrinfo(result);
        return VoidResult<std::string>::error("socket() failed: " + std::string(strerror(errno)));
    }

    // Set TCP_NODELAY for low latency.
    int flag = 1;
    setsockopt(socket_fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    // Set send/recv buffer sizes to 256KB.
    int bufsize = 256 * 1024;
    setsockopt(socket_fd_, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
    setsockopt(socket_fd_, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));

    if (::connect(socket_fd_, result->ai_addr, result->ai_addrlen) < 0) {
        freeaddrinfo(result);
        ::close(socket_fd_);
        socket_fd_ = -1;
        return VoidResult<std::string>::error("connect() failed: " + std::string(strerror(errno)));
    }

    freeaddrinfo(result);
    progress_.start_time = std::chrono::steady_clock::now();
    return VoidResult<std::string>::ok();
}

VoidResult<std::string> Transfer::listen(uint16_t port) {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_fd_ < 0) {
        return VoidResult<std::string>::error("socket() failed: " + std::string(strerror(errno)));
    }

    int reuse = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return VoidResult<std::string>::error("bind() failed: " + std::string(strerror(errno)));
    }

    if (::listen(listen_fd_, 1) < 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return VoidResult<std::string>::error("listen() failed: " + std::string(strerror(errno)));
    }

    // Accept one connection.
    struct sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);
    socket_fd_ = accept(listen_fd_, reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);
    if (socket_fd_ < 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return VoidResult<std::string>::error("accept() failed: " + std::string(strerror(errno)));
    }

    int flag = 1;
    setsockopt(socket_fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    progress_.start_time = std::chrono::steady_clock::now();
    return VoidResult<std::string>::ok();
}

VoidResult<std::string> Transfer::send_all(const void* buf, size_t len) {
    const uint8_t* ptr = static_cast<const uint8_t*>(buf);
    size_t remaining = len;

    while (remaining > 0) {
        ssize_t sent = ::send(socket_fd_, ptr, remaining, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) continue;
            return VoidResult<std::string>::error("send() failed: " + std::string(strerror(errno)));
        }
        if (sent == 0) {
            return VoidResult<std::string>::error("connection closed");
        }
        ptr += sent;
        remaining -= static_cast<size_t>(sent);
    }
    return VoidResult<std::string>::ok();
}

VoidResult<std::string> Transfer::recv_all(void* buf, size_t len) {
    uint8_t* ptr = static_cast<uint8_t*>(buf);
    size_t remaining = len;

    while (remaining > 0) {
        ssize_t received = ::recv(socket_fd_, ptr, remaining, 0);
        if (received < 0) {
            if (errno == EINTR) continue;
            return VoidResult<std::string>::error("recv() failed: " + std::string(strerror(errno)));
        }
        if (received == 0) {
            return VoidResult<std::string>::error("connection closed");
        }
        ptr += received;
        remaining -= static_cast<size_t>(received);
    }
    return VoidResult<std::string>::ok();
}

VoidResult<std::string> Transfer::send_chunk(const std::vector<uint8_t>& data,
                                               uint8_t chunk_type) {
    // Compress the data.
    auto compressed = compress(data);
    bool use_compressed = compressed.size() < data.size();
    const auto& payload = use_compressed ? compressed : data;

    // Build header.
    ChunkHeader hdr{};
    hdr.magic = ChunkHeader::kMagic;
    hdr.sequence = next_sequence_++;
    hdr.compressed_size = static_cast<uint32_t>(payload.size());
    hdr.original_size = static_cast<uint32_t>(data.size());
    hdr.checksum = compute_crc32(payload.data(), payload.size());
    hdr.chunk_type = chunk_type;
    hdr.flags = use_compressed ? 0x01 : 0x00;
    hdr.reserved = 0;

    // Send header.
    auto hdr_result = send_all(&hdr, sizeof(hdr));
    if (!hdr_result) return hdr_result;

    // Send payload.
    auto payload_result = send_all(payload.data(), payload.size());
    if (!payload_result) return payload_result;

    // Apply bandwidth throttling.
    throttle(sizeof(hdr) + payload.size());

    // Update progress.
    {
        std::lock_guard<std::mutex> lock(progress_mutex_);
        progress_.transferred_bytes += data.size();
        progress_.compressed_bytes += payload.size();
        if (progress_.transferred_bytes > 0) {
            progress_.compression_ratio =
                static_cast<double>(progress_.compressed_bytes) /
                static_cast<double>(progress_.transferred_bytes);
        }
        double elapsed = progress_.elapsed_seconds();
        if (elapsed > 0.0) {
            progress_.throughput_mbps =
                (static_cast<double>(progress_.compressed_bytes) * 8.0) /
                (elapsed * 1e6);
        }
    }

    return VoidResult<std::string>::ok();
}

Result<std::vector<uint8_t>, std::string> Transfer::recv_chunk(uint8_t& chunk_type_out) {
    // Receive header.
    ChunkHeader hdr{};
    auto hdr_result = recv_all(&hdr, sizeof(hdr));
    if (!hdr_result) {
        return Result<std::vector<uint8_t>, std::string>::error(hdr_result.err());
    }

    if (hdr.magic != ChunkHeader::kMagic) {
        return Result<std::vector<uint8_t>, std::string>::error("invalid chunk magic");
    }

    chunk_type_out = hdr.chunk_type;

    // Receive payload.
    std::vector<uint8_t> payload(hdr.compressed_size);
    auto payload_result = recv_all(payload.data(), hdr.compressed_size);
    if (!payload_result) {
        return Result<std::vector<uint8_t>, std::string>::error(payload_result.err());
    }

    // Verify checksum.
    uint32_t checksum = compute_crc32(payload.data(), payload.size());
    if (checksum != hdr.checksum) {
        return Result<std::vector<uint8_t>, std::string>::error(
            "checksum mismatch: expected " + std::to_string(hdr.checksum) +
            " got " + std::to_string(checksum));
    }

    // Decompress if needed.
    std::vector<uint8_t> data;
    if (hdr.flags & 0x01) {
        data = decompress(payload, hdr.original_size);
    } else {
        data = std::move(payload);
    }

    // Update progress.
    {
        std::lock_guard<std::mutex> lock(progress_mutex_);
        progress_.transferred_bytes += data.size();
        progress_.compressed_bytes += hdr.compressed_size;
    }

    return Result<std::vector<uint8_t>, std::string>::ok(std::move(data));
}

VoidResult<std::string> Transfer::send_raw(const void* data, size_t len) {
    return send_all(data, len);
}

Result<std::vector<uint8_t>, std::string> Transfer::recv_raw(size_t len) {
    std::vector<uint8_t> buf(len);
    auto result = recv_all(buf.data(), len);
    if (!result) {
        return Result<std::vector<uint8_t>, std::string>::error(result.err());
    }
    return Result<std::vector<uint8_t>, std::string>::ok(std::move(buf));
}

void Transfer::close() {
    if (socket_fd_ >= 0) {
        ::close(socket_fd_);
        socket_fd_ = -1;
    }
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

TransferProgress Transfer::progress() const {
    std::lock_guard<std::mutex> lock(progress_mutex_);
    return progress_;
}

void Transfer::throttle(size_t bytes_sent) {
    if (bandwidth_limit_mbps_ <= 0.0) return;

    double bits = static_cast<double>(bytes_sent) * 8.0;
    double max_bits_per_sec = bandwidth_limit_mbps_ * 1e6;
    double time_needed = bits / max_bits_per_sec;

    if (time_needed > 0.001) {
        std::this_thread::sleep_for(
            std::chrono::microseconds(static_cast<int64_t>(time_needed * 1e6)));
    }
}

} // namespace straylight::mirror
