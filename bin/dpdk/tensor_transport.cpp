// bin/dpdk/tensor_transport.cpp
#include "tensor_transport.h"

#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>

#include <algorithm>
#include <cstring>

namespace straylight::dpdk {

void TensorTransport::set_ethertype(uint16_t ethertype) noexcept {
    ethertype_ = ethertype;
}

void TensorTransport::set_mempool(struct rte_mempool* pool) noexcept {
    mempool_ = pool;
}

void TensorTransport::set_mtu(uint16_t mtu) noexcept {
    mtu_ = mtu;
}

uint16_t TensorTransport::payload_per_fragment() const noexcept {
    // mtu_ represents the maximum L3/L4 payload (excluding Ethernet header).
    // Within that payload we place our TensorFragmentHeader + actual tensor data.
    auto hdr_sz = static_cast<uint16_t>(sizeof(TensorFragmentHeader));
    if (mtu_ <= hdr_sz) {
        return 1; // degenerate but safe
    }
    return mtu_ - hdr_sz;
}

Result<void, std::string> TensorTransport::send_tensor(uint16_t port_id,
                                                        const void* tensor_data,
                                                        size_t size,
                                                        const uint8_t dst_mac[6]) {
    if (!tensor_data || size == 0) {
        return Result<void, std::string>::error("Invalid tensor data");
    }
    if (!mempool_) {
        return Result<void, std::string>::error("No mempool set; call set_mempool() first");
    }

    // Calculate fragmentation
    uint16_t max_payload = mtu_ - static_cast<uint16_t>(sizeof(TensorFragmentHeader));
    if (max_payload == 0) {
        return Result<void, std::string>::error("MTU too small for tensor header");
    }

    uint32_t total_size = static_cast<uint32_t>(size);
    uint16_t num_fragments = static_cast<uint16_t>(
        (total_size + max_payload - 1) / max_payload);

    uint64_t tensor_id = next_tensor_id_++;
    const auto* src = static_cast<const uint8_t*>(tensor_data);

    // Get source MAC address from the port
    struct rte_ether_addr src_mac;
    int ret = rte_eth_macaddr_get(port_id, &src_mac);
    if (ret != 0) {
        return Result<void, std::string>::error(
            "Failed to get MAC for port " + std::to_string(port_id));
    }

    // Build and send fragments in bursts
    static constexpr uint16_t BURST_SIZE = 32;
    struct rte_mbuf* burst[BURST_SIZE];
    uint16_t burst_count = 0;

    for (uint16_t frag_idx = 0; frag_idx < num_fragments; ++frag_idx) {
        uint32_t offset = static_cast<uint32_t>(frag_idx) * max_payload;
        uint16_t frag_payload = static_cast<uint16_t>(
            std::min(static_cast<uint32_t>(max_payload), total_size - offset));

        struct rte_mbuf* mbuf = rte_pktmbuf_alloc(mempool_);
        if (!mbuf) {
            // Free any already-allocated mbufs in the current burst
            for (uint16_t j = 0; j < burst_count; ++j) {
                rte_pktmbuf_free(burst[j]);
            }
            return Result<void, std::string>::error("Failed to allocate mbuf");
        }

        // Build Ethernet header
        auto* eth_hdr = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr*);
        std::memcpy(&eth_hdr->dst_addr, dst_mac, 6);
        std::memcpy(&eth_hdr->src_addr, &src_mac, 6);
        eth_hdr->ether_type = rte_cpu_to_be_16(ethertype_);

        // Build tensor fragment header
        auto* tensor_hdr = reinterpret_cast<TensorFragmentHeader*>(
            reinterpret_cast<uint8_t*>(eth_hdr) + ETH_HEADER_SIZE);
        tensor_hdr->tensor_id       = tensor_id;
        tensor_hdr->total_size      = total_size;
        tensor_hdr->fragment_offset = offset;
        tensor_hdr->fragment_size   = frag_payload;
        tensor_hdr->total_fragments = num_fragments;
        tensor_hdr->fragment_index  = frag_idx;
        tensor_hdr->reserved        = 0;

        // Copy tensor payload
        auto* payload = reinterpret_cast<uint8_t*>(tensor_hdr) +
                        sizeof(TensorFragmentHeader);
        std::memcpy(payload, src + offset, frag_payload);

        // Set mbuf length
        uint16_t total_pkt_len = ETH_HEADER_SIZE +
                                 static_cast<uint16_t>(sizeof(TensorFragmentHeader)) +
                                 frag_payload;
        mbuf->data_len = total_pkt_len;
        mbuf->pkt_len  = total_pkt_len;

        burst[burst_count++] = mbuf;

        // Flush burst if full or last fragment
        if (burst_count == BURST_SIZE || frag_idx == num_fragments - 1) {
            uint16_t sent = rte_eth_tx_burst(port_id, 0, burst, burst_count);
            if (sent < burst_count) {
                for (uint16_t j = sent; j < burst_count; ++j) {
                    rte_pktmbuf_free(burst[j]);
                }
                return Result<void, std::string>::error(
                    "TX burst incomplete: sent " + std::to_string(sent) +
                    " of " + std::to_string(burst_count));
            }
            burst_count = 0;
        }
    }

    return Result<void, std::string>::ok();
}

Result<size_t, std::string> TensorTransport::recv_tensor(uint16_t port_id,
                                                          void* buf,
                                                          size_t max_size) {
    if (!buf || max_size == 0) {
        return Result<size_t, std::string>::error("Invalid receive buffer");
    }

    static constexpr uint16_t BURST_SIZE = 32;
    struct rte_mbuf* pkts[BURST_SIZE];

    // Receive a burst
    uint16_t nb_rx = rte_eth_rx_burst(port_id, 0, pkts, BURST_SIZE);

    for (uint16_t i = 0; i < nb_rx; ++i) {
        struct rte_mbuf* mbuf = pkts[i];
        uint16_t pkt_len = rte_pktmbuf_data_len(mbuf);

        // Minimum viable packet: ETH + tensor header
        uint16_t min_len = ETH_HEADER_SIZE +
                           static_cast<uint16_t>(sizeof(TensorFragmentHeader));
        if (pkt_len < min_len) {
            rte_pktmbuf_free(mbuf);
            continue;
        }

        auto* eth_hdr = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr*);
        uint16_t etype = rte_be_to_cpu_16(eth_hdr->ether_type);
        if (etype != ethertype_) {
            rte_pktmbuf_free(mbuf);
            continue;
        }

        auto* tensor_hdr = reinterpret_cast<const TensorFragmentHeader*>(
            reinterpret_cast<const uint8_t*>(eth_hdr) + ETH_HEADER_SIZE);

        uint64_t tid = tensor_hdr->tensor_id;

        // Find or create reassembly state
        auto& state = reassembly_[tid];
        if (state.total_size == 0) {
            // First fragment of this tensor
            state.total_size      = tensor_hdr->total_size;
            state.total_fragments = tensor_hdr->total_fragments;
            state.received_count  = 0;
            state.data.resize(state.total_size, 0);
            state.received_bitmap.resize(state.total_fragments, false);
        }

        // Record this fragment
        uint16_t frag_idx = tensor_hdr->fragment_index;
        if (frag_idx < state.total_fragments && !state.received_bitmap[frag_idx]) {
            uint32_t offset = tensor_hdr->fragment_offset;
            uint16_t frag_size = tensor_hdr->fragment_size;

            if (offset + frag_size <= state.total_size) {
                const auto* payload = reinterpret_cast<const uint8_t*>(tensor_hdr) +
                                      sizeof(TensorFragmentHeader);
                std::memcpy(state.data.data() + offset, payload, frag_size);
                state.received_bitmap[frag_idx] = true;
                state.received_count++;
            }
        }

        rte_pktmbuf_free(mbuf);

        // Check if tensor is fully reassembled
        if (state.received_count == state.total_fragments) {
            size_t copy_len = std::min(static_cast<size_t>(state.total_size), max_size);
            std::memcpy(buf, state.data.data(), copy_len);
            reassembly_.erase(tid);
            return Result<size_t, std::string>::ok(copy_len);
        }
    }

    // Not yet fully reassembled
    return Result<size_t, std::string>::ok(size_t{0});
}

} // namespace straylight::dpdk
