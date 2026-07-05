// apps/hub/network_panel.h
// Network management panel — interfaces, traffic graphs, active connections.
#pragma once

#include <imgui.h>

#include <cstdint>
#include <string>
#include <vector>

namespace straylight::hub {

struct NetConnection {
    std::string protocol;     // tcp, tcp6, udp, udp6
    std::string local_addr;
    std::string remote_addr;
    std::string state;        // ESTABLISHED, LISTEN, TIME_WAIT, etc.
    int pid = 0;
    std::string process_name;
};

struct InterfaceDetail {
    std::string name;
    std::string ipv4;
    std::string ipv6;
    std::string mac;
    int mtu = 0;
    std::string speed;  // "1000Mb/s" etc.
    bool up = false;

    uint64_t rx_bytes = 0;
    uint64_t tx_bytes = 0;
    uint64_t rx_packets = 0;
    uint64_t tx_packets = 0;
    uint64_t rx_errors = 0;
    uint64_t tx_errors = 0;
    uint64_t rx_dropped = 0;
    uint64_t tx_dropped = 0;

    // Previous samples for rate calculation
    uint64_t prev_rx_bytes = 0;
    uint64_t prev_tx_bytes = 0;
    float rx_rate_mbps = 0.0f;
    float tx_rate_mbps = 0.0f;

    // Traffic history
    static constexpr int HISTORY_LEN = 120;
    float rx_history[HISTORY_LEN]{};
    float tx_history[HISTORY_LEN]{};
    int history_idx = 0;
};

class NetworkPanel {
public:
    NetworkPanel() = default;

    /// Sample network data.
    void sample();

    /// Render the network tab.
    void render();

private:
    std::vector<InterfaceDetail> interfaces_;
    std::vector<NetConnection> connections_;
    bool first_sample_ = true;

    // Sorting state for connections table
    int sort_column_ = 0;
    bool sort_ascending_ = true;

    // Filter
    char filter_buf_[128]{};

    void sample_interfaces();
    void sample_connections();
    void render_interface(InterfaceDetail& iface);
    void render_connections_table();
};

} // namespace straylight::hub
