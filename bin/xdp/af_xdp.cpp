// bin/xdp/af_xdp.cpp
#include "af_xdp.h"

// libbpf 0.5 ships xsk.h under bpf/; libxdp places it under xdp/
#if __has_include(<xdp/xsk.h>)
#  include <xdp/xsk.h>
#else
#  include <bpf/xsk.h>
#endif
#include <linux/if_xdp.h>
#include <poll.h>

#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <utility>

namespace straylight::xdp {

/// Internal ring storage so we don't leak libxdp types into the header.
struct AfXdpSocket::Rings {
    struct xsk_ring_cons rx;
    struct xsk_ring_prod tx;
    struct xsk_ring_prod fill;
    struct xsk_ring_cons comp;
};

AfXdpSocket::AfXdpSocket() = default;

AfXdpSocket::~AfXdpSocket() { cleanup(); }

AfXdpSocket::AfXdpSocket(AfXdpSocket&& other) noexcept
    : xsk_(other.xsk_),
      umem_(other.umem_),
      umem_area_(other.umem_area_),
      umem_size_(other.umem_size_),
      rings_(other.rings_),
      frame_freelist_(other.frame_freelist_),
      frame_freelist_sz_(other.frame_freelist_sz_),
      frame_freelist_top_(other.frame_freelist_top_) {
    other.xsk_                = nullptr;
    other.umem_               = nullptr;
    other.umem_area_          = nullptr;
    other.umem_size_          = 0;
    other.rings_              = nullptr;
    other.frame_freelist_     = nullptr;
    other.frame_freelist_sz_  = 0;
    other.frame_freelist_top_ = 0;
}

AfXdpSocket& AfXdpSocket::operator=(AfXdpSocket&& other) noexcept {
    if (this != &other) {
        cleanup();
        xsk_                = other.xsk_;
        umem_               = other.umem_;
        umem_area_          = other.umem_area_;
        umem_size_          = other.umem_size_;
        rings_              = other.rings_;
        frame_freelist_     = other.frame_freelist_;
        frame_freelist_sz_  = other.frame_freelist_sz_;
        frame_freelist_top_ = other.frame_freelist_top_;
        other.xsk_                = nullptr;
        other.umem_               = nullptr;
        other.umem_area_          = nullptr;
        other.umem_size_          = 0;
        other.rings_              = nullptr;
        other.frame_freelist_     = nullptr;
        other.frame_freelist_sz_  = 0;
        other.frame_freelist_top_ = 0;
    }
    return *this;
}

Result<void, std::string> AfXdpSocket::create(const std::string& ifname,
                                               uint32_t queue_id,
                                               uint32_t frame_count) {
    if (xsk_) {
        return Result<void, std::string>::error("AF_XDP socket already created");
    }
    if (frame_count == 0) {
        return Result<void, std::string>::error("frame_count must be > 0");
    }

    // Allocate UMEM area (page-aligned via mmap)
    umem_size_ = static_cast<size_t>(frame_count) * FRAME_SIZE;
    umem_area_ = mmap(nullptr, umem_size_,
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (umem_area_ == MAP_FAILED) {
        umem_area_ = nullptr;
        return Result<void, std::string>::error(
            std::string("mmap failed for UMEM: ") + std::strerror(errno));
    }

    // Allocate ring storage
    rings_ = new Rings{};

    // Create UMEM
    struct xsk_umem_config umem_cfg = {};
    umem_cfg.fill_size      = frame_count;
    umem_cfg.comp_size      = frame_count;
    umem_cfg.frame_size     = FRAME_SIZE;
    umem_cfg.frame_headroom = 0;
    umem_cfg.flags          = 0;

    int ret = xsk_umem__create(&umem_, umem_area_, umem_size_,
                               &rings_->fill, &rings_->comp, &umem_cfg);
    if (ret) {
        delete rings_;
        rings_ = nullptr;
        munmap(umem_area_, umem_size_);
        umem_area_ = nullptr;
        return Result<void, std::string>::error(
            std::string("xsk_umem__create failed: ") + std::strerror(-ret));
    }

    // Build frame freelist
    frame_freelist_sz_  = frame_count;
    frame_freelist_     = new uint64_t[frame_count];
    frame_freelist_top_ = frame_count;
    for (uint32_t i = 0; i < frame_count; ++i) {
        frame_freelist_[i] = static_cast<uint64_t>(i) * FRAME_SIZE;
    }

    // Create XSK socket
    struct xsk_socket_config xsk_cfg = {};
    xsk_cfg.rx_size      = frame_count;
    xsk_cfg.tx_size      = frame_count;
    xsk_cfg.xdp_flags    = 0;
    xsk_cfg.bind_flags   = 0;
    xsk_cfg.libbpf_flags = 0;

    ret = xsk_socket__create(&xsk_, ifname.c_str(), queue_id,
                             umem_, &rings_->rx, &rings_->tx, &xsk_cfg);
    if (ret) {
        delete[] frame_freelist_;
        frame_freelist_ = nullptr;
        xsk_umem__delete(umem_);
        umem_ = nullptr;
        delete rings_;
        rings_ = nullptr;
        munmap(umem_area_, umem_size_);
        umem_area_ = nullptr;
        return Result<void, std::string>::error(
            std::string("xsk_socket__create failed: ") + std::strerror(-ret));
    }

    // Populate the fill ring so the kernel has frames to write received packets into
    uint32_t idx = 0;
    uint32_t reserved = xsk_ring_prod__reserve(&rings_->fill, frame_count / 2, &idx);
    for (uint32_t i = 0; i < reserved; ++i) {
        *xsk_ring_prod__fill_addr(&rings_->fill, idx + i) = alloc_frame();
    }
    if (reserved > 0) {
        xsk_ring_prod__submit(&rings_->fill, reserved);
    }

    return Result<void, std::string>::ok();
}

Result<void, std::string> AfXdpSocket::send(const void* data, size_t len) {
    if (!xsk_) {
        return Result<void, std::string>::error("AF_XDP socket not created");
    }
    if (!data || len == 0) {
        return Result<void, std::string>::error("Invalid send buffer");
    }
    if (len > FRAME_SIZE) {
        return Result<void, std::string>::error("Data exceeds frame size");
    }

    // Reclaim any completed TX frames first
    reclaim_completed_tx();

    uint64_t frame_addr = alloc_frame();
    if (frame_addr == INVALID_UMEM_FRAME) {
        return Result<void, std::string>::error("No free UMEM frames for TX");
    }

    // Copy payload into the UMEM frame
    auto* dst = static_cast<uint8_t*>(umem_area_) + frame_addr;
    std::memcpy(dst, data, len);

    // Reserve a TX ring slot
    uint32_t tx_idx = 0;
    uint32_t reserved = xsk_ring_prod__reserve(&rings_->tx, 1, &tx_idx);
    if (reserved == 0) {
        free_frame(frame_addr);
        return Result<void, std::string>::error("TX ring full");
    }

    // Fill the TX descriptor
    struct xdp_desc* desc = xsk_ring_prod__tx_desc(&rings_->tx, tx_idx);
    desc->addr = frame_addr;
    desc->len  = static_cast<uint32_t>(len);

    xsk_ring_prod__submit(&rings_->tx, 1);

    // Kick the kernel to send
    int fd = xsk_socket__fd(xsk_);
    int ret = ::sendto(fd, nullptr, 0, MSG_DONTWAIT, nullptr, 0);
    if (ret < 0 && errno != EAGAIN && errno != ENOBUFS) {
        return Result<void, std::string>::error(
            std::string("sendto kick failed: ") + std::strerror(errno));
    }

    return Result<void, std::string>::ok();
}

Result<size_t, std::string> AfXdpSocket::recv(void* buf, size_t max_len) {
    if (!xsk_) {
        return Result<size_t, std::string>::error("AF_XDP socket not created");
    }
    if (!buf || max_len == 0) {
        return Result<size_t, std::string>::error("Invalid receive buffer");
    }

    int fd = xsk_socket__fd(xsk_);

    // Poll for readiness with a short timeout
    struct pollfd pfd = {};
    pfd.fd     = fd;
    pfd.events = POLLIN;

    int ready = ::poll(&pfd, 1, 100);
    if (ready < 0) {
        return Result<size_t, std::string>::error(
            std::string("poll failed: ") + std::strerror(errno));
    }
    if (ready == 0) {
        return Result<size_t, std::string>::ok(size_t{0});
    }

    // Peek at the RX ring
    uint32_t rx_idx = 0;
    uint32_t received = xsk_ring_cons__peek(&rings_->rx, 1, &rx_idx);
    if (received == 0) {
        return Result<size_t, std::string>::ok(size_t{0});
    }

    // Extract the packet from the RX descriptor
    const struct xdp_desc* desc = xsk_ring_cons__rx_desc(&rings_->rx, rx_idx);
    uint64_t addr = desc->addr;
    uint32_t pkt_len = desc->len;

    size_t copy_len = std::min(static_cast<size_t>(pkt_len), max_len);
    auto* src = static_cast<const uint8_t*>(umem_area_) + addr;
    std::memcpy(buf, src, copy_len);

    // Release the RX slot
    xsk_ring_cons__release(&rings_->rx, 1);

    // Re-stock the fill ring with the freed frame
    uint32_t fill_idx = 0;
    if (xsk_ring_prod__reserve(&rings_->fill, 1, &fill_idx) == 1) {
        *xsk_ring_prod__fill_addr(&rings_->fill, fill_idx) = addr;
        xsk_ring_prod__submit(&rings_->fill, 1);
    } else {
        free_frame(addr);
    }

    return Result<size_t, std::string>::ok(copy_len);
}

void AfXdpSocket::close() {
    cleanup();
}

bool AfXdpSocket::is_open() const noexcept {
    return xsk_ != nullptr;
}

uint64_t AfXdpSocket::alloc_frame() {
    if (frame_freelist_top_ == 0) {
        return INVALID_UMEM_FRAME;
    }
    return frame_freelist_[--frame_freelist_top_];
}

void AfXdpSocket::free_frame(uint64_t addr) {
    if (frame_freelist_top_ < frame_freelist_sz_) {
        frame_freelist_[frame_freelist_top_++] = addr;
    }
}

void AfXdpSocket::reclaim_completed_tx() {
    uint32_t comp_idx = 0;
    uint32_t completed = xsk_ring_cons__peek(&rings_->comp, frame_freelist_sz_, &comp_idx);
    for (uint32_t i = 0; i < completed; ++i) {
        uint64_t addr = *xsk_ring_cons__comp_addr(&rings_->comp, comp_idx + i);
        free_frame(addr);
    }
    if (completed > 0) {
        xsk_ring_cons__release(&rings_->comp, completed);
    }
}

void AfXdpSocket::cleanup() noexcept {
    if (xsk_) {
        xsk_socket__delete(xsk_);
        xsk_ = nullptr;
    }
    if (umem_) {
        xsk_umem__delete(umem_);
        umem_ = nullptr;
    }
    if (umem_area_) {
        munmap(umem_area_, umem_size_);
        umem_area_ = nullptr;
        umem_size_ = 0;
    }
    delete rings_;
    rings_ = nullptr;
    delete[] frame_freelist_;
    frame_freelist_     = nullptr;
    frame_freelist_sz_  = 0;
    frame_freelist_top_ = 0;
}

} // namespace straylight::xdp
