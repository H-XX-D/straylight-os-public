/**
 * StrayLight Bridge Network Sync — Implementation.
 */

#include "network_sync.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace straylight::bridge {

// ---------------------------------------------------------------------------
// CRC32 table (same as mirror/transfer.cpp)
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
    0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7822, 0x9b64c2b0, 0xec63f226,
    0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f, 0x72076785, 0x05005713,
    0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38, 0x92d28e9b, 0xe5d5be0d,
    0x7cdcefb7, 0x0bdbdf21, 0x86d3d2d4, 0xf1d4e242, 0x68ddb3f6, 0x1fda8360,
    0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777, 0x88085ae6, 0xff0f6b70,
    0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69, 0x616bffd3, 0x166ccf45,
    0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2, 0xa7672661, 0xd06016f7,
    0x4969474d, 0x3e6e77db, 0xaed16a4a, 0xd9d65adc, 0x40df0b66, 0x37d83bf0,
    0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9, 0xbdbdf21c, 0xcabac28a,
    0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd706ff, 0x54de5729, 0x23d967bf,
    0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94, 0xb40bbe37, 0xc30c8ea1,
    0x5a05df1b, 0x2d02ef8d, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000
};

uint32_t NetworkSync::crc32(const void* data, size_t len) {
    const uint8_t* buf = static_cast<const uint8_t*>(data);
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc = crc32_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

// ---------------------------------------------------------------------------
// Whisper encryption (stream cipher placeholder)
// ---------------------------------------------------------------------------

void NetworkSync::whisper_encrypt(uint8_t* data, size_t len, uint64_t key) {
    // XOR-based stream cipher with key scheduling.
    // Production would use ChaCha20-Poly1305.
    uint64_t state = key;
    for (size_t i = 0; i < len; ++i) {
        // Simple xorshift64 PRNG.
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        data[i] ^= static_cast<uint8_t>(state & 0xFF);
    }
}

void NetworkSync::whisper_decrypt(uint8_t* data, size_t len, uint64_t key) {
    // XOR cipher is its own inverse.
    whisper_encrypt(data, len, key);
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

NetworkSync::NetworkSync() {
    std::memset(&stats_, 0, sizeof(stats_));
}

NetworkSync::~NetworkSync() {
    close();
}

// ---------------------------------------------------------------------------
// Connection management
// ---------------------------------------------------------------------------

VoidResult<std::string> NetworkSync::connect(const std::string& host, uint16_t port) {
    struct addrinfo hints{}, *result = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

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

    int flag = 1;
    setsockopt(socket_fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    if (::connect(socket_fd_, result->ai_addr, result->ai_addrlen) < 0) {
        freeaddrinfo(result);
        ::close(socket_fd_);
        socket_fd_ = -1;
        return VoidResult<std::string>::error("connect() failed: " + std::string(strerror(errno)));
    }

    freeaddrinfo(result);
    return VoidResult<std::string>::ok();
}

VoidResult<std::string> NetworkSync::listen(uint16_t port) {
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

    if (::listen(listen_fd_, 4) < 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return VoidResult<std::string>::error("listen() failed: " + std::string(strerror(errno)));
    }

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

    return VoidResult<std::string>::ok();
}

VoidResult<std::string> NetworkSync::send_all(const void* buf, size_t len) {
    const uint8_t* ptr = static_cast<const uint8_t*>(buf);
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t sent = ::send(socket_fd_, ptr, remaining, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) continue;
            return VoidResult<std::string>::error("send failed: " + std::string(strerror(errno)));
        }
        if (sent == 0) {
            return VoidResult<std::string>::error("connection closed");
        }
        ptr += sent;
        remaining -= static_cast<size_t>(sent);
    }
    return VoidResult<std::string>::ok();
}

VoidResult<std::string> NetworkSync::recv_all(void* buf, size_t len) {
    uint8_t* ptr = static_cast<uint8_t*>(buf);
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t received = ::recv(socket_fd_, ptr, remaining, 0);
        if (received < 0) {
            if (errno == EINTR) continue;
            return VoidResult<std::string>::error("recv failed: " + std::string(strerror(errno)));
        }
        if (received == 0) {
            return VoidResult<std::string>::error("connection closed");
        }
        ptr += received;
        remaining -= static_cast<size_t>(received);
    }
    return VoidResult<std::string>::ok();
}

void NetworkSync::close() {
    if (socket_fd_ >= 0) {
        ::close(socket_fd_);
        socket_fd_ = -1;
    }
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

// ---------------------------------------------------------------------------
// Delta compression
// ---------------------------------------------------------------------------

std::vector<uint8_t> NetworkSync::compute_page_delta(const uint8_t* old_data,
                                                       const uint8_t* new_data,
                                                       size_t page_size) {
    // XOR delta: only store non-zero XOR bytes with their offsets.
    // Format: [num_runs:4] { [offset:2][length:2][data...] }*
    std::vector<uint8_t> delta;

    std::vector<std::pair<uint16_t, uint16_t>> runs; // (offset, length)
    size_t i = 0;
    while (i < page_size) {
        // Skip matching bytes.
        while (i < page_size && old_data[i] == new_data[i]) ++i;
        if (i >= page_size) break;

        // Start of a changed run.
        uint16_t run_start = static_cast<uint16_t>(i);
        while (i < page_size && old_data[i] != new_data[i]) ++i;
        uint16_t run_len = static_cast<uint16_t>(i - run_start);

        runs.emplace_back(run_start, run_len);
    }

    // Serialize.
    uint32_t num_runs = static_cast<uint32_t>(runs.size());
    delta.resize(4);
    std::memcpy(delta.data(), &num_runs, 4);

    for (const auto& [offset, length] : runs) {
        size_t pos = delta.size();
        delta.resize(pos + 4 + length);
        std::memcpy(delta.data() + pos, &offset, 2);
        std::memcpy(delta.data() + pos + 2, &length, 2);
        std::memcpy(delta.data() + pos + 4, new_data + offset, length);
    }

    return delta;
}

void NetworkSync::apply_page_delta(uint8_t* page_data,
                                     const std::vector<uint8_t>& delta,
                                     size_t page_size) {
    if (delta.size() < 4) return;

    uint32_t num_runs = 0;
    std::memcpy(&num_runs, delta.data(), 4);

    size_t pos = 4;
    for (uint32_t r = 0; r < num_runs && pos + 4 <= delta.size(); ++r) {
        uint16_t offset = 0, length = 0;
        std::memcpy(&offset, delta.data() + pos, 2);
        std::memcpy(&length, delta.data() + pos + 2, 2);
        pos += 4;

        if (offset + length <= page_size && pos + length <= delta.size()) {
            std::memcpy(page_data + offset, delta.data() + pos, length);
        }
        pos += length;
    }
}

// ---------------------------------------------------------------------------
// Page sync
// ---------------------------------------------------------------------------

VoidResult<std::string> NetworkSync::sync_pages(uint32_t bridge_id,
                                                  const std::vector<DirtyPage>& pages,
                                                  const void* base_addr) {
    if (pages.empty()) return VoidResult<std::string>::ok();

    auto sync_start = std::chrono::steady_clock::now();
    const uint8_t* base = static_cast<const uint8_t*>(base_addr);

    // Build payload: all page data concatenated.
    std::vector<uint8_t> payload;

    for (const auto& page : pages) {
        // Page entry: [offset:8][size:4][data...]
        uint64_t offset = page.offset;
        uint32_t size = static_cast<uint32_t>(page.size);

        size_t pos = payload.size();
        payload.resize(pos + 12 + size);
        std::memcpy(payload.data() + pos, &offset, 8);
        std::memcpy(payload.data() + pos + 8, &size, 4);

        // Check if we have a shadow copy for delta compression.
        size_t page_idx = page.offset / page.size;
        if (page_idx < page_shadows_.size() && !page_shadows_[page_idx].empty()) {
            auto delta = compute_page_delta(
                page_shadows_[page_idx].data(),
                base + page.offset,
                page.size);

            // Use delta if it's smaller than the full page.
            if (delta.size() < page.size) {
                // Re-encode with delta flag.
                payload.resize(pos);
                uint32_t delta_size = static_cast<uint32_t>(delta.size());
                payload.resize(pos + 12 + delta_size);
                std::memcpy(payload.data() + pos, &offset, 8);
                // Set high bit of size to indicate delta.
                uint32_t flagged_size = delta_size | 0x80000000;
                std::memcpy(payload.data() + pos + 8, &flagged_size, 4);
                std::memcpy(payload.data() + pos + 12, delta.data(), delta_size);

                std::lock_guard<std::mutex> lock(stats_mutex_);
                stats_.total_bytes_saved_delta += (page.size - delta.size());
            } else {
                std::memcpy(payload.data() + pos + 12, base + page.offset, size);
            }
        } else {
            std::memcpy(payload.data() + pos + 12, base + page.offset, size);
        }

        // Update shadow copy.
        if (page_idx >= page_shadows_.size()) {
            page_shadows_.resize(page_idx + 1);
        }
        page_shadows_[page_idx].assign(base + page.offset,
                                         base + page.offset + page.size);
    }

    // Apply encryption if enabled.
    if (encryption_enabled_) {
        // Use bridge_id as encryption key seed (production would use proper key exchange).
        whisper_encrypt(payload.data(), payload.size(),
                        static_cast<uint64_t>(bridge_id) * 0x9E3779B97F4A7C15ULL);
    }

    // Build header.
    PageSyncHeader hdr{};
    hdr.magic = PageSyncHeader::kMagic;
    hdr.bridge_id = bridge_id;
    hdr.sequence = sync_sequence_++;
    hdr.num_pages = static_cast<uint32_t>(pages.size());
    hdr.total_size = static_cast<uint32_t>(payload.size());
    hdr.checksum = crc32(payload.data(), payload.size());
    hdr.flags = encryption_enabled_ ? 0x02 : 0x00;

    // Send header + payload.
    auto hdr_result = send_all(&hdr, sizeof(hdr));
    if (!hdr_result) return hdr_result;

    auto payload_result = send_all(payload.data(), payload.size());
    if (!payload_result) return payload_result;

    auto sync_end = std::chrono::steady_clock::now();
    double latency_ms = std::chrono::duration<double, std::milli>(sync_end - sync_start).count();

    // Update stats.
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.total_syncs++;
        stats_.total_pages_sent += pages.size();
        stats_.total_bytes_sent += payload.size();
        stats_.last_sync_time = sync_end;

        // Running average latency.
        stats_.avg_sync_latency_ms =
            (stats_.avg_sync_latency_ms * (stats_.total_syncs - 1) + latency_ms) /
            stats_.total_syncs;
    }

    return VoidResult<std::string>::ok();
}

Result<uint32_t, std::string> NetworkSync::receive_and_apply(void* base_addr,
                                                               size_t region_size) {
    uint8_t* base = static_cast<uint8_t*>(base_addr);

    // Receive header.
    PageSyncHeader hdr{};
    auto hdr_result = recv_all(&hdr, sizeof(hdr));
    if (!hdr_result) {
        return Result<uint32_t, std::string>::error(hdr_result.err());
    }

    if (hdr.magic != PageSyncHeader::kMagic) {
        return Result<uint32_t, std::string>::error("invalid sync header magic");
    }

    // Receive payload.
    std::vector<uint8_t> payload(hdr.total_size);
    auto payload_result = recv_all(payload.data(), hdr.total_size);
    if (!payload_result) {
        return Result<uint32_t, std::string>::error(payload_result.err());
    }

    // Verify checksum.
    uint32_t computed_crc = crc32(payload.data(), payload.size());
    if (computed_crc != hdr.checksum) {
        return Result<uint32_t, std::string>::error("checksum mismatch");
    }

    // Decrypt if needed.
    if (hdr.flags & 0x02) {
        whisper_decrypt(payload.data(), payload.size(),
                        static_cast<uint64_t>(hdr.bridge_id) * 0x9E3779B97F4A7C15ULL);
    }

    // Apply pages.
    size_t pos = 0;
    uint32_t applied = 0;

    while (pos + 12 <= payload.size()) {
        uint64_t offset = 0;
        uint32_t size = 0;
        std::memcpy(&offset, payload.data() + pos, 8);
        std::memcpy(&size, payload.data() + pos + 8, 4);
        pos += 12;

        bool is_delta = (size & 0x80000000) != 0;
        size &= 0x7FFFFFFF;

        if (pos + size > payload.size()) break;
        if (offset + (is_delta ? 0 : size) > region_size) {
            pos += size;
            continue;
        }

        if (is_delta) {
            // Apply delta.
            std::vector<uint8_t> delta(payload.data() + pos, payload.data() + pos + size);
            apply_page_delta(base + offset, delta, 4096); // Assume 4K pages
        } else {
            // Direct copy.
            std::memcpy(base + offset, payload.data() + pos, size);
        }

        pos += size;
        ++applied;
    }

    return Result<uint32_t, std::string>::ok(applied);
}

SyncStats NetworkSync::get_stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

} // namespace straylight::bridge
