// apps/widgets/hpc/network_topology.h
#pragma once

#include <straylight/widget.h>
#include <array>
#include <string>
#include <vector>

namespace straylight::widgets {

struct NicInfo {
    std::string name;       // e.g. "eth0", "ib0"
    std::string driver;
    std::string speed;      // e.g. "100Gb/s"
    bool link_up = false;
    uint64_t rx_bytes = 0;
    uint64_t tx_bytes = 0;
    uint64_t rx_packets = 0;
    uint64_t tx_packets = 0;
    uint64_t rx_errors = 0;
    uint64_t tx_errors = 0;
    // Bandwidth deltas
    float rx_bw_mbps = 0.0f;
    float tx_bw_mbps = 0.0f;
    uint64_t prev_rx_bytes = 0;
    uint64_t prev_tx_bytes = 0;
    // RDMA stats
    bool rdma_capable = false;
    uint64_t rdma_send_wrs = 0;
    uint64_t rdma_recv_wrs = 0;
    // History
    static constexpr int kHistLen = 120;
    std::array<float, kHistLen> rx_bw_hist{};
    std::array<float, kHistLen> tx_bw_hist{};
    int hist_offset = 0;
};

class NetworkTopologyWidget : public WidgetBase {
public:
    const char* name() const override { return "Network Topology"; }
    float poll_interval() const override { return 1.0f; }
    void update() override;
    void render(bool* p_open) override;

private:
    std::vector<NicInfo> nics_;
    int selected_nic_ = 0;
    std::chrono::steady_clock::time_point last_bw_sample_{};

    void read_sysfs_nics();
    void compute_bandwidth();
    void push_history(NicInfo& nic);
    static std::string human_bytes(uint64_t bytes);
    static std::string human_bw(float mbps);
};

} // namespace straylight::widgets
