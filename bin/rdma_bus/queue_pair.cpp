// bin/rdma_bus/queue_pair.cpp
#include "queue_pair.h"
#include "verbs.h"

#include <infiniband/verbs.h>

#include <cerrno>
#include <cstring>
#include <poll.h>

namespace straylight::rdma {

QueuePairManager::QueuePairManager(VerbsContext& verbs)
    : verbs_(verbs) {}

QueuePairManager::~QueuePairManager() {
    for (auto& [num, entry] : qps_) {
        if (entry.qp) {
            ibv_destroy_qp(entry.qp);
            entry.qp = nullptr;
        }
    }
}

Result<uint32_t, std::string> QueuePairManager::create_qp(int type,
                                                           uint32_t send_depth,
                                                           uint32_t recv_depth) {
    if (!verbs_.pd()) {
        return Result<uint32_t, std::string>::error("PD not created");
    }
    if (!verbs_.cq()) {
        return Result<uint32_t, std::string>::error("CQ not created");
    }

    struct ibv_qp_init_attr qp_init_attr = {};
    qp_init_attr.send_cq             = verbs_.cq();
    qp_init_attr.recv_cq             = verbs_.cq();
    qp_init_attr.qp_type             = static_cast<enum ibv_qp_type>(type);
    qp_init_attr.cap.max_send_wr     = send_depth;
    qp_init_attr.cap.max_recv_wr     = recv_depth;
    qp_init_attr.cap.max_send_sge    = 1;
    qp_init_attr.cap.max_recv_sge    = 1;
    qp_init_attr.sq_sig_all          = 1;

    struct ibv_qp* qp = ibv_create_qp(verbs_.pd(), &qp_init_attr);
    if (!qp) {
        return Result<uint32_t, std::string>::error(
            std::string("ibv_create_qp failed: ") + std::strerror(errno));
    }

    uint32_t qp_num = qp->qp_num;
    qps_[qp_num] = QpEntry{qp, qp_num};

    return Result<uint32_t, std::string>::ok(qp_num);
}

Result<void, std::string> QueuePairManager::modify_to_init(uint32_t qp_num,
                                                            uint16_t port_num) {
    struct ibv_qp* qp = find_qp(qp_num);
    if (!qp) {
        return Result<void, std::string>::error(
            "QP " + std::to_string(qp_num) + " not found");
    }

    struct ibv_qp_attr attr = {};
    attr.qp_state        = IBV_QPS_INIT;
    attr.pkey_index      = 0;
    attr.port_num        = port_num;
    attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE |
                           IBV_ACCESS_REMOTE_WRITE |
                           IBV_ACCESS_REMOTE_READ;

    int flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
    int ret = ibv_modify_qp(qp, &attr, flags);
    if (ret) {
        return Result<void, std::string>::error(
            "modify_to_init failed: " + std::string(std::strerror(ret)));
    }

    return Result<void, std::string>::ok();
}

Result<void, std::string> QueuePairManager::modify_to_rtr(uint32_t qp_num,
                                                           const RemoteQpInfo& remote_info) {
    struct ibv_qp* qp = find_qp(qp_num);
    if (!qp) {
        return Result<void, std::string>::error(
            "QP " + std::to_string(qp_num) + " not found");
    }

    struct ibv_qp_attr attr = {};
    attr.qp_state              = IBV_QPS_RTR;
    attr.path_mtu              = IBV_MTU_4096;
    attr.dest_qp_num           = remote_info.qp_num;
    attr.rq_psn                = 0;
    attr.max_dest_rd_atomic    = 1;
    attr.min_rnr_timer         = 12;

    // Set up address handle for the path
    attr.ah_attr.dlid          = remote_info.lid;
    attr.ah_attr.sl            = 0;
    attr.ah_attr.src_path_bits = 0;
    attr.ah_attr.port_num      = 1;

    // Check if GRH is needed (RoCE / IPv6 fabric)
    bool use_grh = false;
    for (int i = 0; i < 16; ++i) {
        if (remote_info.gid[i] != 0) {
            use_grh = true;
            break;
        }
    }

    if (use_grh) {
        attr.ah_attr.is_global     = 1;
        attr.ah_attr.grh.hop_limit = 64;
        attr.ah_attr.grh.sgid_index = 0;
        std::memcpy(&attr.ah_attr.grh.dgid, remote_info.gid, 16);
    }

    int flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;

    int ret = ibv_modify_qp(qp, &attr, flags);
    if (ret) {
        return Result<void, std::string>::error(
            "modify_to_rtr failed: " + std::string(std::strerror(ret)));
    }

    return Result<void, std::string>::ok();
}

Result<void, std::string> QueuePairManager::modify_to_rts(uint32_t qp_num) {
    struct ibv_qp* qp = find_qp(qp_num);
    if (!qp) {
        return Result<void, std::string>::error(
            "QP " + std::to_string(qp_num) + " not found");
    }

    struct ibv_qp_attr attr = {};
    attr.qp_state      = IBV_QPS_RTS;
    attr.timeout        = 14;  // ~67 seconds
    attr.retry_cnt      = 7;
    attr.rnr_retry      = 7;
    attr.sq_psn         = 0;
    attr.max_rd_atomic  = 1;

    int flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;

    int ret = ibv_modify_qp(qp, &attr, flags);
    if (ret) {
        return Result<void, std::string>::error(
            "modify_to_rts failed: " + std::string(std::strerror(ret)));
    }

    return Result<void, std::string>::ok();
}

Result<void, std::string> QueuePairManager::post_send(uint32_t qp_num,
                                                       const SendWR& wr) {
    struct ibv_qp* qp = find_qp(qp_num);
    if (!qp) {
        return Result<void, std::string>::error(
            "QP " + std::to_string(qp_num) + " not found");
    }

    struct ibv_sge sge = {};
    sge.addr   = reinterpret_cast<uint64_t>(wr.buf);
    sge.length = wr.length;
    sge.lkey   = wr.lkey;

    struct ibv_send_wr send_wr = {};
    send_wr.wr_id      = reinterpret_cast<uint64_t>(wr.buf);
    send_wr.sg_list    = &sge;
    send_wr.num_sge    = 1;
    send_wr.send_flags = wr.signaled ? IBV_SEND_SIGNALED : 0;

    if (wr.rdma_write) {
        send_wr.opcode                = IBV_WR_RDMA_WRITE;
        send_wr.wr.rdma.remote_addr   = wr.remote_addr;
        send_wr.wr.rdma.rkey          = wr.remote_rkey;
    } else if (wr.rdma_read) {
        send_wr.opcode                = IBV_WR_RDMA_READ;
        send_wr.wr.rdma.remote_addr   = wr.remote_addr;
        send_wr.wr.rdma.rkey          = wr.remote_rkey;
    } else {
        send_wr.opcode = IBV_WR_SEND;
    }

    struct ibv_send_wr* bad_wr = nullptr;
    int ret = ibv_post_send(qp, &send_wr, &bad_wr);
    if (ret) {
        return Result<void, std::string>::error(
            "ibv_post_send failed: " + std::string(std::strerror(ret)));
    }

    return Result<void, std::string>::ok();
}

Result<void, std::string> QueuePairManager::post_recv(uint32_t qp_num,
                                                       const RecvWR& wr) {
    struct ibv_qp* qp = find_qp(qp_num);
    if (!qp) {
        return Result<void, std::string>::error(
            "QP " + std::to_string(qp_num) + " not found");
    }

    struct ibv_sge sge = {};
    sge.addr   = reinterpret_cast<uint64_t>(wr.buf);
    sge.length = wr.length;
    sge.lkey   = wr.lkey;

    struct ibv_recv_wr recv_wr = {};
    recv_wr.wr_id   = reinterpret_cast<uint64_t>(wr.buf);
    recv_wr.sg_list = &sge;
    recv_wr.num_sge = 1;

    struct ibv_recv_wr* bad_wr = nullptr;
    int ret = ibv_post_recv(qp, &recv_wr, &bad_wr);
    if (ret) {
        return Result<void, std::string>::error(
            "ibv_post_recv failed: " + std::string(std::strerror(ret)));
    }

    return Result<void, std::string>::ok();
}

Result<std::vector<CompletionEntry>, std::string>
QueuePairManager::poll_cq(int timeout_ms) {
    if (!verbs_.cq()) {
        return Result<std::vector<CompletionEntry>, std::string>::error("CQ not created");
    }

    // First try a non-blocking poll
    static constexpr int MAX_WC = 16;
    struct ibv_wc wc[MAX_WC];
    int n = ibv_poll_cq(verbs_.cq(), MAX_WC, wc);

    if (n < 0) {
        return Result<std::vector<CompletionEntry>, std::string>::error(
            "ibv_poll_cq failed");
    }

    if (n == 0 && timeout_ms > 0) {
        // Wait for a completion event on the comp channel
        int fd = verbs_.cq()->channel ? verbs_.cq()->channel->fd : -1;
        if (fd >= 0) {
            struct pollfd pfd = {};
            pfd.fd     = fd;
            pfd.events = POLLIN;
            int ready = ::poll(&pfd, 1, timeout_ms);
            if (ready > 0) {
                // Ack the event
                struct ibv_cq* ev_cq = nullptr;
                void* ev_ctx = nullptr;
                ibv_get_cq_event(verbs_.cq()->channel, &ev_cq, &ev_ctx);
                ibv_ack_cq_events(ev_cq, 1);
                ibv_req_notify_cq(verbs_.cq(), 0);

                // Retry poll
                n = ibv_poll_cq(verbs_.cq(), MAX_WC, wc);
                if (n < 0) {
                    return Result<std::vector<CompletionEntry>, std::string>::error(
                        "ibv_poll_cq failed after event");
                }
            }
        }
    }

    std::vector<CompletionEntry> entries;
    entries.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        CompletionEntry ce;
        ce.wr_id    = wc[i].wr_id;
        ce.status   = wc[i].status;
        ce.byte_len = wc[i].byte_len;
        ce.is_send  = (wc[i].opcode & IBV_WC_RECV) == 0;
        entries.push_back(ce);
    }

    return Result<std::vector<CompletionEntry>, std::string>::ok(std::move(entries));
}

uint32_t QueuePairManager::qp_count() const noexcept {
    return static_cast<uint32_t>(qps_.size());
}

struct ibv_qp* QueuePairManager::find_qp(uint32_t qp_num) const {
    auto it = qps_.find(qp_num);
    if (it == qps_.end()) {
        return nullptr;
    }
    return it->second.qp;
}

} // namespace straylight::rdma
