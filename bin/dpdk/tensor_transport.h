// bin/dpdk/tensor_transport.h
#pragma once

#include <straylight/result.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct rte_mempool;

namespace straylight::dpdk {

/// Header prepended to each tensor fragment transmitted via DPDK.
struct TensorFragmentHeader {
    uint64_t tensor_id;       // Unique ID for the tensor transfer
    uint32_t total_size;      // Total tensor size in bytes
    uint32_t fragment_offset; // Byte offset of this fragment within the tensor
    uint16_t fragment_size;   // Size of payload in this fragment
    uint16_t total_fragments; // Total number of fragments
    uint16_t fragment_index;  // Index of this fragment (0-based)
    uint16_t reserved;        // Alignment padding
} __attribute__((packed));

/// Tensor-over-DPDK transport: fragments large tensors into MTU-sized mbufs,
/// transmits them, and reassembles on the receive side.
class TensorTransport {
public:
    TensorTransport() = default;
    ~TensorTransport() = default;

    /// Send a tensor (arbitrary data blob) to a destination MAC address via a DPDK port.
    /// The tensor is fragmented into MTU-sized packets and sent as a burst.
    Result<void, std::string> send_tensor(uint16_t port_id,
                                          const void* tensor_data,
                                          size_t size,
                                          const uint8_t dst_mac[6]);

    /// Receive and reassemble a tensor from a DPDK port.
    /// Returns the number of bytes reassembled into buf.
    Result<size_t, std::string> recv_tensor(uint16_t port_id,
                                            void* buf,
                                            size_t max_size);

    /// Set the EtherType used for tensor transport frames.
    void set_ethertype(uint16_t ethertype) noexcept;

    /// Set the mempool to use for TX mbuf allocation.
    void set_mempool(struct rte_mempool* pool) noexcept;

    /// Set the MTU (payload per fragment, excluding headers). Default: 1500.
    void set_mtu(uint16_t mtu) noexcept;

private:
    static constexpr uint16_t DEFAULT_MTU       = 1500;
    static constexpr uint16_t ETH_HEADER_SIZE   = 14;
    static constexpr uint16_t DEFAULT_ETHERTYPE = 0x88B5; // IEEE 802 local experimental

    uint16_t           mtu_       = DEFAULT_MTU;
    uint16_t           ethertype_ = DEFAULT_ETHERTYPE;
    struct rte_mempool* mempool_  = nullptr;
    uint64_t           next_tensor_id_ = 1;

    /// Reassembly buffer for in-progress tensors
    struct ReassemblyState {
        std::vector<uint8_t> data;
        uint32_t             total_size       = 0;
        uint16_t             total_fragments  = 0;
        uint16_t             received_count   = 0;
        std::vector<bool>    received_bitmap;
    };

    std::unordered_map<uint64_t, ReassemblyState> reassembly_;

    /// Maximum payload per fragment (MTU minus our header)
    [[nodiscard]] uint16_t payload_per_fragment() const noexcept;
};

} // namespace straylight::dpdk
