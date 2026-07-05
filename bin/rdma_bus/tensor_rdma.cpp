// bin/rdma_bus/tensor_rdma.cpp
#include "tensor_rdma.h"
#include "verbs.h"
#include "queue_pair.h"
#include "memory_region.h"

#include <infiniband/verbs.h>

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

namespace straylight::rdma {

/// Wire-format for QP info exchange over TCP.
struct QpExchangeData {
    uint32_t qp_num;
    uint16_t lid;
    uint8_t  gid[16];
} __attribute__((packed));

TensorRdma::TensorRdma(VerbsContext& verbs,
                         QueuePairManager& qp_mgr,
                         MemoryRegionManager& mr_mgr)
    : verbs_(verbs),
      qp_mgr_(qp_mgr),
      mr_mgr_(mr_mgr) {}

Result<void, std::string> TensorRdma::connect(const std::string& remote_addr,
                                                uint16_t remote_port) {
    if (connected_) {
        return Result<void, std::string>::error("Already connected");
    }

    // Create a QP for this connection
    auto qp_res = qp_mgr_.create_qp(IBV_QPT_RC, 128, 128);
    if (!qp_res.has_value()) {
        return Result<void, std::string>::error("Failed to create QP: " + qp_res.error());
    }
    local_qp_num_ = qp_res.value();

    // Transition to INIT
    auto init_res = qp_mgr_.modify_to_init(local_qp_num_);
    if (!init_res.has_value()) {
        return Result<void, std::string>::error("Failed to move QP to INIT: " + init_res.error());
    }

    // Connect via TCP to exchange QP info
    struct addrinfo hints = {};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    std::string port_str = std::to_string(remote_port);
    struct addrinfo* res = nullptr;
    int gai_err = getaddrinfo(remote_addr.c_str(), port_str.c_str(), &hints, &res);
    if (gai_err != 0) {
        return Result<void, std::string>::error(
            std::string("getaddrinfo failed: ") + gai_strerror(gai_err));
    }

    tcp_sock_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (tcp_sock_ < 0) {
        freeaddrinfo(res);
        return Result<void, std::string>::error(
            std::string("socket() failed: ") + std::strerror(errno));
    }

    if (::connect(tcp_sock_, res->ai_addr, res->ai_addrlen) < 0) {
        freeaddrinfo(res);
        ::close(tcp_sock_);
        tcp_sock_ = -1;
        return Result<void, std::string>::error(
            std::string("connect() failed: ") + std::strerror(errno));
    }
    freeaddrinfo(res);

    // Exchange QP info
    auto exch_res = exchange_qp_info(tcp_sock_);
    if (!exch_res.has_value()) {
        ::close(tcp_sock_);
        tcp_sock_ = -1;
        return Result<void, std::string>::error(exch_res.error());
    }

    connected_ = true;
    return Result<void, std::string>::ok();
}

Result<void, std::string> TensorRdma::exchange_qp_info(int sock) {
    // Get local port info
    struct ibv_port_attr port_attr = {};
    int ret = ibv_query_port(verbs_.context(), 1, &port_attr);
    if (ret) {
        return Result<void, std::string>::error(
            "ibv_query_port failed: " + std::string(std::strerror(ret)));
    }

    // Get local GID
    union ibv_gid local_gid = {};
    ret = ibv_query_gid(verbs_.context(), 1, 0, &local_gid);
    if (ret) {
        return Result<void, std::string>::error(
            "ibv_query_gid failed: " + std::string(std::strerror(ret)));
    }

    // Build local exchange data
    QpExchangeData local_data = {};
    local_data.qp_num = local_qp_num_;
    local_data.lid    = port_attr.lid;
    std::memcpy(local_data.gid, &local_gid, 16);

    // Send local data
    ssize_t sent = ::send(sock, &local_data, sizeof(local_data), 0);
    if (sent != sizeof(local_data)) {
        return Result<void, std::string>::error("Failed to send QP exchange data");
    }

    // Receive remote data
    QpExchangeData remote_data = {};
    ssize_t recvd = ::recv(sock, &remote_data, sizeof(remote_data), MSG_WAITALL);
    if (recvd != sizeof(remote_data)) {
        return Result<void, std::string>::error("Failed to receive QP exchange data");
    }

    // Build RemoteQpInfo and transition QP
    RemoteQpInfo remote_info = {};
    remote_info.qp_num = remote_data.qp_num;
    remote_info.lid    = remote_data.lid;
    std::memcpy(remote_info.gid, remote_data.gid, 16);

    // Transition to RTR
    auto rtr_res = qp_mgr_.modify_to_rtr(local_qp_num_, remote_info);
    if (!rtr_res.has_value()) {
        return Result<void, std::string>::error("Failed to move QP to RTR: " + rtr_res.error());
    }

    // Transition to RTS
    auto rts_res = qp_mgr_.modify_to_rts(local_qp_num_);
    if (!rts_res.has_value()) {
        return Result<void, std::string>::error("Failed to move QP to RTS: " + rts_res.error());
    }

    return Result<void, std::string>::ok();
}

Result<void, std::string> TensorRdma::write_tensor(uint32_t local_mr_handle,
                                                     uint64_t remote_addr,
                                                     uint32_t remote_rkey,
                                                     size_t size) {
    if (!connected_) {
        return Result<void, std::string>::error("Not connected");
    }

    const RegionInfo* info = mr_mgr_.get_info(local_mr_handle);
    if (!info) {
        return Result<void, std::string>::error(
            "Local MR handle " + std::to_string(local_mr_handle) + " not found");
    }
    if (size > info->size) {
        return Result<void, std::string>::error("Write size exceeds registered region");
    }

    SendWR wr = {};
    wr.buf         = info->addr;
    wr.length      = static_cast<uint32_t>(size);
    wr.lkey        = info->lkey;
    wr.remote_addr = remote_addr;
    wr.remote_rkey = remote_rkey;
    wr.rdma_write  = true;
    wr.signaled    = true;

    auto post_res = qp_mgr_.post_send(local_qp_num_, wr);
    if (!post_res.has_value()) {
        return Result<void, std::string>::error("post_send failed: " + post_res.error());
    }

    return wait_completion();
}

Result<void, std::string> TensorRdma::read_tensor(uint64_t remote_addr,
                                                    uint32_t remote_rkey,
                                                    uint32_t local_mr_handle,
                                                    size_t size) {
    if (!connected_) {
        return Result<void, std::string>::error("Not connected");
    }

    const RegionInfo* info = mr_mgr_.get_info(local_mr_handle);
    if (!info) {
        return Result<void, std::string>::error(
            "Local MR handle " + std::to_string(local_mr_handle) + " not found");
    }
    if (size > info->size) {
        return Result<void, std::string>::error("Read size exceeds registered region");
    }

    SendWR wr = {};
    wr.buf         = info->addr;
    wr.length      = static_cast<uint32_t>(size);
    wr.lkey        = info->lkey;
    wr.remote_addr = remote_addr;
    wr.remote_rkey = remote_rkey;
    wr.rdma_read   = true;
    wr.signaled    = true;

    auto post_res = qp_mgr_.post_send(local_qp_num_, wr);
    if (!post_res.has_value()) {
        return Result<void, std::string>::error("post_send (read) failed: " + post_res.error());
    }

    return wait_completion();
}

Result<void, std::string> TensorRdma::wait_completion(int timeout_ms) {
    auto poll_res = qp_mgr_.poll_cq(timeout_ms);
    if (!poll_res.has_value()) {
        return Result<void, std::string>::error("CQ poll failed: " + poll_res.error());
    }

    const auto& completions = poll_res.value();
    if (completions.empty()) {
        return Result<void, std::string>::error("RDMA operation timed out");
    }

    for (const auto& ce : completions) {
        if (ce.status != 0) { // IBV_WC_SUCCESS == 0
            return Result<void, std::string>::error(
                "RDMA WC error, status=" + std::to_string(ce.status));
        }
    }

    return Result<void, std::string>::ok();
}

bool TensorRdma::connected() const noexcept {
    return connected_;
}

uint32_t TensorRdma::local_qp_num() const noexcept {
    return local_qp_num_;
}

} // namespace straylight::rdma
