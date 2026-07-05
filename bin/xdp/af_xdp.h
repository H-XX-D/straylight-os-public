// bin/xdp/af_xdp.h
#pragma once

#include <straylight/result.h>

#include <cstddef>
#include <cstdint>
#include <string>

struct xsk_socket;
struct xsk_umem;
struct xsk_ring_cons;
struct xsk_ring_prod;

namespace straylight::xdp {

/// AF_XDP socket — high-performance userspace packet I/O via XDP.
class AfXdpSocket {
public:
    AfXdpSocket();
    ~AfXdpSocket();

    AfXdpSocket(const AfXdpSocket&) = delete;
    AfXdpSocket& operator=(const AfXdpSocket&) = delete;
    AfXdpSocket(AfXdpSocket&& other) noexcept;
    AfXdpSocket& operator=(AfXdpSocket&& other) noexcept;

    /// Create and bind an AF_XDP socket to the given interface and queue.
    Result<void, std::string> create(const std::string& ifname,
                                     uint32_t queue_id,
                                     uint32_t frame_count);

    /// Transmit data through the AF_XDP socket.
    Result<void, std::string> send(const void* data, size_t len);

    /// Receive data from the AF_XDP socket. Returns the number of bytes received.
    Result<size_t, std::string> recv(void* buf, size_t max_len);

    /// Close and tear down the socket.
    void close();

    /// Whether the socket is open and ready.
    [[nodiscard]] bool is_open() const noexcept;

private:
    static constexpr size_t FRAME_SIZE = 4096;
    static constexpr uint64_t INVALID_UMEM_FRAME = UINT64_MAX;

    struct xsk_socket* xsk_    = nullptr;
    struct xsk_umem*   umem_   = nullptr;
    void*              umem_area_ = nullptr;
    size_t             umem_size_ = 0;

    // XSK rings — heap-allocated to avoid exposing xdp headers in .h
    struct Rings;
    Rings* rings_ = nullptr;

    // Frame allocator for UMEM
    uint64_t* frame_freelist_    = nullptr;
    uint32_t  frame_freelist_sz_ = 0;
    uint32_t  frame_freelist_top_= 0;

    uint64_t alloc_frame();
    void     free_frame(uint64_t addr);
    void     cleanup() noexcept;
    void     reclaim_completed_tx();
};

} // namespace straylight::xdp
