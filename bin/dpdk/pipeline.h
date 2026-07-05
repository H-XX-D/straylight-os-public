// bin/dpdk/pipeline.h
#pragma once

#include <straylight/result.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct rte_mbuf;

namespace straylight::dpdk {

/// Statistics for the processing pipeline.
struct PipelineStats {
    uint64_t total_rx       = 0;
    uint64_t total_tx       = 0;
    uint64_t total_dropped  = 0;
    uint64_t stages_run     = 0;
};

/// A processing stage receives a burst of mbufs and returns how many to forward.
/// The handler may modify, drop, or inject packets.
/// Return value: number of packets to forward (may be <= nb_pkts).
using StageHandler = std::function<uint16_t(struct rte_mbuf** pkts, uint16_t nb_pkts)>;

/// Multi-stage packet processing pipeline over DPDK ports.
class Pipeline {
public:
    Pipeline() = default;

    /// Add a named processing stage to the pipeline (executed in order).
    Result<void, std::string> add_stage(const std::string& name, StageHandler handler);

    /// Receive a burst from (port_id, queue_id), run all stages, then transmit.
    /// Returns the number of packets successfully transmitted.
    Result<uint32_t, std::string> process_burst(uint16_t port_id,
                                                uint16_t queue_id,
                                                uint16_t batch_size);

    /// Return cumulative pipeline statistics.
    [[nodiscard]] PipelineStats stats() const noexcept;

    /// Reset statistics to zero.
    void reset_stats() noexcept;

    /// Number of stages in the pipeline.
    [[nodiscard]] size_t stage_count() const noexcept;

private:
    struct Stage {
        std::string  name;
        StageHandler handler;
    };

    std::vector<Stage> stages_;
    PipelineStats      stats_{};

    static constexpr uint16_t MAX_BURST = 512;
};

} // namespace straylight::dpdk
