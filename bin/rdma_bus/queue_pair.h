// bin/rdma_bus/queue_pair.h
#pragma once

#include <straylight/result.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct ibv_qp;

namespace straylight::rdma {

class VerbsContext; // forward declaration

/// Identifies the remote endpoint for QP connection.
struct RemoteQpInfo {
    uint32_t qp_num;
    uint16_t lid;
    uint8_t  gid[16];
};

/// Send work request descriptor.
struct SendWR {
    void*    buf;
    uint32_t length;
    uint32_t lkey;
    uint64_t remote_addr;  // for RDMA write/read
    uint32_t remote_rkey;  // for RDMA write/read
    bool     rdma_write = false;
    bool     rdma_read  = false;
    bool     signaled   = true;
};

/// Receive work request descriptor.
struct RecvWR {
    void*    buf;
    uint32_t length;
    uint32_t lkey;
};

/// Completion queue entry.
struct CompletionEntry {
    uint64_t wr_id;
    uint32_t status;     // ibv_wc_status
    uint32_t byte_len;
    bool     is_send;
};

/// Manages RDMA queue pairs — create, transition through states, post work requests.
class QueuePairManager {
public:
    explicit QueuePairManager(VerbsContext& verbs);
    ~QueuePairManager();

    QueuePairManager(const QueuePairManager&) = delete;
    QueuePairManager& operator=(const QueuePairManager&) = delete;

    /// Create a QP. type: IBV_QPT_RC, IBV_QPT_UD, etc.
    Result<uint32_t, std::string> create_qp(int type,
                                            uint32_t send_depth,
                                            uint32_t recv_depth);

    /// Transition QP to INIT state (required before RTR).
    Result<void, std::string> modify_to_init(uint32_t qp_num, uint16_t port_num = 1);

    /// Transition QP to RTR (Ready-to-Receive) using remote endpoint info.
    Result<void, std::string> modify_to_rtr(uint32_t qp_num,
                                            const RemoteQpInfo& remote_info);

    /// Transition QP to RTS (Ready-to-Send).
    Result<void, std::string> modify_to_rts(uint32_t qp_num);

    /// Post a send work request (send, RDMA write, or RDMA read).
    Result<void, std::string> post_send(uint32_t qp_num, const SendWR& wr);

    /// Post a receive work request.
    Result<void, std::string> post_recv(uint32_t qp_num, const RecvWR& wr);

    /// Poll the completion queue. Returns completed entries.
    Result<std::vector<CompletionEntry>, std::string> poll_cq(int timeout_ms);

    /// Get a QP's number from internal tracking (by creation order index).
    [[nodiscard]] uint32_t qp_count() const noexcept;

private:
    VerbsContext& verbs_;

    struct QpEntry {
        struct ibv_qp* qp;
        uint32_t       qp_num;
    };

    std::unordered_map<uint32_t, QpEntry> qps_; // keyed by qp_num

    struct ibv_qp* find_qp(uint32_t qp_num) const;
};

} // namespace straylight::rdma
