// services/ghost/page_server.cpp
// Page server implementation — serves memory pages with zstd compression.

#include "page_server.h"

#include <straylight/log.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

// zstd-like compression (using built-in simple RLE for portability,
// replaced by real zstd when linked)
#ifdef HAVE_ZSTD
#include <zstd.h>
#endif

namespace straylight {

PageServer::PageServer() = default;

PageServer::~PageServer() {
    stop();
}

Result<void, SLError> PageServer::start(pid_t pid, uint16_t port) {
    if (running_.load()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::AlreadyExists, "Page server already running"});
    }

    source_pid_ = pid;

    // Create TCP server socket
    server_fd_ = ::socket(AF_INET6, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError,
                    std::string("socket() failed: ") + ::strerror(errno)});
    }

    int opt = 1;
    ::setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Enable TCP_NODELAY for low-latency page serving
    ::setsockopt(server_fd_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    struct sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(port);
    addr.sin6_addr = in6addr_any;

    if (::bind(server_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        int e = errno;
        ::close(server_fd_);
        server_fd_ = -1;
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError,
                    std::string("bind() failed: ") + ::strerror(e)});
    }

    if (::listen(server_fd_, 8) < 0) {
        int e = errno;
        ::close(server_fd_);
        server_fd_ = -1;
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError,
                    std::string("listen() failed: ") + ::strerror(e)});
    }

    running_.store(true);
    pages_served_.store(0);
    bytes_sent_.store(0);

    server_thread_ = std::thread(&PageServer::serve_loop, this);

    SL_INFO("ghost: page server started on port {} for pid {}", port, pid);
    return Result<void, SLError>::ok();
}

void PageServer::stop() {
    running_.store(false);

    if (server_fd_ >= 0) {
        ::shutdown(server_fd_, SHUT_RDWR);
        ::close(server_fd_);
        server_fd_ = -1;
    }

    if (server_thread_.joinable()) {
        server_thread_.join();
    }
}

void PageServer::serve_loop() {
    while (running_.load()) {
        struct pollfd pfd{};
        pfd.fd = server_fd_;
        pfd.events = POLLIN;
        int rc = ::poll(&pfd, 1, 500);

        if (rc <= 0) continue;

        struct sockaddr_in6 client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = ::accept(server_fd_,
                                 reinterpret_cast<struct sockaddr*>(&client_addr),
                                 &addr_len);
        if (client_fd < 0) {
            if (errno != EINTR && errno != EAGAIN) {
                SL_WARN("ghost: accept() failed: {}", ::strerror(errno));
            }
            continue;
        }

        // Handle client in the same thread (pages are served sequentially)
        handle_client(client_fd);
        ::close(client_fd);
    }
}

void PageServer::handle_client(int client_fd) {
    // Protocol: client sends PageRequest structs, server replies with page data.
    // Request: [addr:u64][count:u64]
    // Response per page: [compressed_size:u32][compressed_data:...]

    while (running_.load()) {
        // Read request
        uint64_t req_buf[2];
        ssize_t n = ::recv(client_fd, req_buf, sizeof(req_buf), MSG_WAITALL);
        if (n <= 0) break; // Client disconnected

        uint64_t addr = req_buf[0];
        uint64_t count = req_buf[1];

        if (count == 0 || count > 256) {
            count = 1;
        }

        for (uint64_t i = 0; i < count; ++i) {
            uint64_t page_addr = addr + i * GHOST_PAGE_SIZE;

            auto page_result = read_page(source_pid_, page_addr);
            if (!page_result.has_value()) {
                // Send zero page on error
                std::vector<uint8_t> zeros(GHOST_PAGE_SIZE, 0);
                auto compressed = compress_page(zeros.data(), zeros.size());
                uint32_t comp_size = static_cast<uint32_t>(compressed.size());
                ::send(client_fd, &comp_size, sizeof(comp_size), 0);
                ::send(client_fd, compressed.data(), compressed.size(), 0);
                continue;
            }

            auto& page_data = page_result.value();
            auto compressed = compress_page(page_data.data(), page_data.size());

            uint32_t comp_size = static_cast<uint32_t>(compressed.size());
            ::send(client_fd, &comp_size, sizeof(comp_size), 0);
            ::send(client_fd, compressed.data(), compressed.size(), 0);

            pages_served_.fetch_add(1);
            bytes_sent_.fetch_add(sizeof(comp_size) + compressed.size());

            // Track access pattern for prefetch
            {
                std::lock_guard lock(pattern_mutex_);
                access_history_.push_back(page_addr);
                if (access_history_.size() > MAX_HISTORY) {
                    access_history_.erase(access_history_.begin());
                }
            }
        }
    }
}

Result<std::vector<uint8_t>, SLError> PageServer::read_page(pid_t pid, uint64_t addr) {
    std::string mem_path = "/proc/" + std::to_string(pid) + "/mem";
    int fd = ::open(mem_path.c_str(), O_RDONLY);
    if (fd < 0) {
        return Result<std::vector<uint8_t>, SLError>::error(
            SLError{SLErrorCode::PermissionDenied,
                    "Cannot open " + mem_path + ": " + ::strerror(errno)});
    }

    std::vector<uint8_t> buf(GHOST_PAGE_SIZE);
    ssize_t n = ::pread(fd, buf.data(), GHOST_PAGE_SIZE, static_cast<off_t>(addr));
    ::close(fd);

    if (n < 0) {
        return Result<std::vector<uint8_t>, SLError>::error(
            SLError{SLErrorCode::IOError,
                    "pread failed at " + std::to_string(addr) + ": " + ::strerror(errno)});
    }

    if (static_cast<size_t>(n) < GHOST_PAGE_SIZE) {
        // Zero-fill remainder
        std::memset(buf.data() + n, 0, GHOST_PAGE_SIZE - static_cast<size_t>(n));
    }

    return Result<std::vector<uint8_t>, SLError>::ok(std::move(buf));
}

Result<std::vector<uint8_t>, SLError> PageServer::read_pages(pid_t pid, uint64_t addr,
                                                              uint64_t count) {
    std::string mem_path = "/proc/" + std::to_string(pid) + "/mem";
    int fd = ::open(mem_path.c_str(), O_RDONLY);
    if (fd < 0) {
        return Result<std::vector<uint8_t>, SLError>::error(
            SLError{SLErrorCode::PermissionDenied,
                    "Cannot open " + mem_path});
    }

    uint64_t total_size = count * GHOST_PAGE_SIZE;
    std::vector<uint8_t> buf(total_size);
    ssize_t n = ::pread(fd, buf.data(), total_size, static_cast<off_t>(addr));
    ::close(fd);

    if (n < 0) {
        return Result<std::vector<uint8_t>, SLError>::error(
            SLError{SLErrorCode::IOError,
                    "pread failed: " + std::string(::strerror(errno))});
    }

    if (static_cast<uint64_t>(n) < total_size) {
        std::memset(buf.data() + n, 0, total_size - static_cast<uint64_t>(n));
    }

    return Result<std::vector<uint8_t>, SLError>::ok(std::move(buf));
}

std::vector<uint8_t> PageServer::compress_page(const uint8_t* data, size_t size,
                                                int level) {
#ifdef HAVE_ZSTD
    size_t bound = ZSTD_compressBound(size);
    std::vector<uint8_t> compressed(bound);
    size_t result = ZSTD_compress(compressed.data(), bound, data, size, level);
    if (ZSTD_isError(result)) {
        // Fallback: send uncompressed
        compressed.resize(size);
        std::memcpy(compressed.data(), data, size);
        return compressed;
    }
    compressed.resize(result);
    return compressed;
#else
    // Simple RLE compression fallback for zero-heavy pages
    std::vector<uint8_t> out;
    out.reserve(size);

    // Header byte: 0x00 = uncompressed, 0x01 = RLE
    size_t zero_count = 0;
    for (size_t i = 0; i < size; ++i) {
        if (data[i] == 0) ++zero_count;
    }

    if (zero_count > size / 2) {
        // RLE encode: [0x01][runs of (byte, count_u16)...]
        out.push_back(0x01);
        size_t i = 0;
        while (i < size) {
            uint8_t byte = data[i];
            uint16_t count = 1;
            while (i + count < size && data[i + count] == byte && count < 65535) {
                ++count;
            }
            out.push_back(byte);
            out.push_back(static_cast<uint8_t>(count & 0xFF));
            out.push_back(static_cast<uint8_t>((count >> 8) & 0xFF));
            i += count;
        }
    } else {
        // Uncompressed
        out.push_back(0x00);
        out.insert(out.end(), data, data + size);
    }

    (void)level;
    return out;
#endif
}

Result<std::vector<uint8_t>, std::string> PageServer::decompress_page(
    const uint8_t* data, size_t size) {
    if (size == 0) {
        return Result<std::vector<uint8_t>, std::string>::error("Empty data");
    }

#ifdef HAVE_ZSTD
    size_t decompressed_size = ZSTD_getFrameContentSize(data, size);
    if (decompressed_size == ZSTD_CONTENTSIZE_ERROR ||
        decompressed_size == ZSTD_CONTENTSIZE_UNKNOWN) {
        return Result<std::vector<uint8_t>, std::string>::error("Invalid zstd frame");
    }
    std::vector<uint8_t> out(decompressed_size);
    size_t result = ZSTD_decompress(out.data(), decompressed_size, data, size);
    if (ZSTD_isError(result)) {
        return Result<std::vector<uint8_t>, std::string>::error(
            std::string("zstd decompress error: ") + ZSTD_getErrorName(result));
    }
    out.resize(result);
    return Result<std::vector<uint8_t>, std::string>::ok(std::move(out));
#else
    if (data[0] == 0x00) {
        // Uncompressed
        std::vector<uint8_t> out(data + 1, data + size);
        return Result<std::vector<uint8_t>, std::string>::ok(std::move(out));
    } else if (data[0] == 0x01) {
        // RLE decode
        std::vector<uint8_t> out;
        out.reserve(GHOST_PAGE_SIZE);
        size_t i = 1;
        while (i + 2 < size) {
            uint8_t byte = data[i];
            uint16_t count = static_cast<uint16_t>(data[i + 1]) |
                             (static_cast<uint16_t>(data[i + 2]) << 8);
            for (uint16_t c = 0; c < count; ++c) {
                out.push_back(byte);
            }
            i += 3;
        }
        return Result<std::vector<uint8_t>, std::string>::ok(std::move(out));
    }

    return Result<std::vector<uint8_t>, std::string>::error("Unknown compression format");
#endif
}

std::vector<uint64_t> PageServer::predict_prefetch(uint64_t current_addr,
                                                    int lookahead) const {
    std::lock_guard lock(pattern_mutex_);
    std::vector<uint64_t> predictions;

    if (access_history_.size() < 2) {
        // No pattern yet — predict sequential access
        for (int i = 1; i <= lookahead; ++i) {
            predictions.push_back(current_addr + static_cast<uint64_t>(i) * GHOST_PAGE_SIZE);
        }
        return predictions;
    }

    // Detect stride pattern from recent history
    int64_t total_stride = 0;
    int stride_count = 0;
    for (size_t i = 1; i < access_history_.size(); ++i) {
        int64_t delta = static_cast<int64_t>(access_history_[i]) -
                        static_cast<int64_t>(access_history_[i - 1]);
        // Only count page-aligned strides
        if (delta % static_cast<int64_t>(GHOST_PAGE_SIZE) == 0) {
            total_stride += delta;
            ++stride_count;
        }
    }

    if (stride_count > 0) {
        int64_t avg_stride = total_stride / stride_count;
        if (avg_stride == 0) avg_stride = static_cast<int64_t>(GHOST_PAGE_SIZE);

        for (int i = 1; i <= lookahead; ++i) {
            uint64_t predicted = static_cast<uint64_t>(
                static_cast<int64_t>(current_addr) + avg_stride * i);
            predictions.push_back(predicted);
        }
    } else {
        // Fallback: sequential
        for (int i = 1; i <= lookahead; ++i) {
            predictions.push_back(current_addr + static_cast<uint64_t>(i) * GHOST_PAGE_SIZE);
        }
    }

    return predictions;
}

} // namespace straylight
