// bin/dpdk/pipeline.cpp
#include "pipeline.h"

#include <rte_ethdev.h>
#include <rte_mbuf.h>

#include <algorithm>

namespace straylight::dpdk {

Result<void, std::string> Pipeline::add_stage(const std::string& name,
                                              StageHandler handler) {
    if (name.empty()) {
        return Result<void, std::string>::error("Stage name cannot be empty");
    }
    if (!handler) {
        return Result<void, std::string>::error("Stage handler cannot be null");
    }

    // Check for duplicate names
    for (const auto& s : stages_) {
        if (s.name == name) {
            return Result<void, std::string>::error(
                "Duplicate stage name: " + name);
        }
    }

    stages_.push_back(Stage{name, std::move(handler)});
    return Result<void, std::string>::ok();
}

Result<uint32_t, std::string> Pipeline::process_burst(uint16_t port_id,
                                                      uint16_t queue_id,
                                                      uint16_t batch_size) {
    if (stages_.empty()) {
        return Result<uint32_t, std::string>::error("No stages in pipeline");
    }

    uint16_t actual_batch = std::min(batch_size, MAX_BURST);
    struct rte_mbuf* pkts[MAX_BURST];

    // Receive a burst of packets
    uint16_t nb_rx = rte_eth_rx_burst(port_id, queue_id, pkts, actual_batch);
    if (nb_rx == 0) {
        return Result<uint32_t, std::string>::ok(uint32_t{0});
    }

    stats_.total_rx += nb_rx;
    uint16_t nb_pkts = nb_rx;

    // Run each stage sequentially
    for (auto& stage : stages_) {
        nb_pkts = stage.handler(pkts, nb_pkts);
        stats_.stages_run++;

        if (nb_pkts == 0) {
            // All packets dropped by this stage
            break;
        }
    }

    if (nb_pkts == 0) {
        stats_.total_dropped += nb_rx;
        return Result<uint32_t, std::string>::ok(uint32_t{0});
    }

    // Transmit surviving packets
    uint16_t nb_tx = rte_eth_tx_burst(port_id, queue_id, pkts, nb_pkts);
    stats_.total_tx += nb_tx;

    // Free any unsent packets
    if (nb_tx < nb_pkts) {
        uint16_t dropped = nb_pkts - nb_tx;
        stats_.total_dropped += dropped;
        for (uint16_t i = nb_tx; i < nb_pkts; ++i) {
            rte_pktmbuf_free(pkts[i]);
        }
    }

    // Also free any packets that were dropped between rx and the final stage count
    uint16_t stage_dropped = nb_rx - nb_pkts;
    stats_.total_dropped += stage_dropped;

    return Result<uint32_t, std::string>::ok(static_cast<uint32_t>(nb_tx));
}

PipelineStats Pipeline::stats() const noexcept {
    return stats_;
}

void Pipeline::reset_stats() noexcept {
    stats_ = PipelineStats{};
}

size_t Pipeline::stage_count() const noexcept {
    return stages_.size();
}

} // namespace straylight::dpdk
