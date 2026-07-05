// bin/rdma_bus/tensor_rdma.h
#pragma once

#include <straylight/result.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace straylight::rdma {

class VerbsContext;
class QueuePairManager;
class MemoryRegionManager;

/// Zero-copy tensor transfer over RDMA (write/read verbs).
class TensorRdma {
public:
    TensorRdma(VerbsContext& verbs,
               QueuePairManager& qp_mgr,
               MemoryRegionManager& mr_mgr);
    ~TensorRdma() = default;

    TensorRdma(const TensorRdma&) = delete;
    TensorRdma& operator=(const TensorRdma&) = delete;

    /// Establish an RDMA connection to a remote peer.
    /// Exchanges QP info over a TCP socket for bootstrapping.
    Result<void, std::string> connect(const std::string& remote_addr,
                                      uint16_t remote_port);

    /// RDMA-write a local tensor to a remote memory region (one-sided push).
    Result<void, std::string> write_tensor(uint32_t local_mr_handle,
                                           uint64_t remote_addr,
                                           uint32_t remote_rkey,
                                           size_t size);

    /// RDMA-read a tensor from a remote memory region into local memory (one-sided pull).
    Result<void, std::string> read_tensor(uint64_t remote_addr,
                                          uint32_t remote_rkey,
                                          uint32_t local_mr_handle,
                                          size_t size);

    /// Whether we have an active RDMA connection.
    [[nodiscard]] bool connected() const noexcept;

    /// The local QP number for the active connection.
    [[nodiscard]] uint32_t local_qp_num() const noexcept;

private:
    VerbsContext&        verbs_;
    QueuePairManager&    qp_mgr_;
    MemoryRegionManager& mr_mgr_;

    uint32_t local_qp_num_  = 0;
    bool     connected_     = false;
    int      tcp_sock_      = -1;

    /// Exchange QP info with the remote over TCP.
    Result<void, std::string> exchange_qp_info(int sock);

    /// Wait for an RDMA operation to complete via CQ poll.
    Result<void, std::string> wait_completion(int timeout_ms = 5000);
};

} // namespace straylight::rdma
