// apps/widgets/hpc/network_topology.cpp
#include "network_topology.h"
#include "widget_registry.h"

#include <imgui.h>

REGISTER_WIDGET(straylight::widgets::NetworkTopologyWidget, "network_topology", "Network Topology", straylight::widgets::WidgetCategory::HPC);
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <algorithm>

namespace straylight::widgets {

std::string NetworkTopologyWidget::human_bytes(uint64_t bytes) {
    char buf[64];
    if (bytes >= 1ULL << 40) std::snprintf(buf, sizeof(buf), "%.2f TiB", static_cast<double>(bytes) / (1ULL << 40));
    else if (bytes >= 1ULL << 30) std::snprintf(buf, sizeof(buf), "%.2f GiB", static_cast<double>(bytes) / (1ULL << 30));
    else if (bytes >= 1ULL << 20) std::snprintf(buf, sizeof(buf), "%.2f MiB", static_cast<double>(bytes) / (1ULL << 20));
    else if (bytes >= 1ULL << 10) std::snprintf(buf, sizeof(buf), "%.2f KiB", static_cast<double>(bytes) / (1ULL << 10));
    else std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    return buf;
}

std::string NetworkTopologyWidget::human_bw(float mbps) {
    char buf[64];
    if (mbps >= 1000.0f) std::snprintf(buf, sizeof(buf), "%.2f Gb/s", mbps / 1000.0f);
    else std::snprintf(buf, sizeof(buf), "%.1f Mb/s", mbps);
    return buf;
}

void NetworkTopologyWidget::read_sysfs_nics() {
    namespace fs = std::filesystem;
    const std::string net_dir = "/sys/class/net";
    if (!fs::exists(net_dir)) return;

    for (auto& entry : fs::directory_iterator(net_dir)) {
        std::string ifname = entry.path().filename().string();
        if (ifname == "lo") continue; // skip loopback

        // Find or create
        NicInfo* nic = nullptr;
        for (auto& n : nics_) {
            if (n.name == ifname) { nic = &n; break; }
        }
        if (!nic) {
            nics_.emplace_back();
            nic = &nics_.back();
            nic->name = ifname;
        }

        std::string base = entry.path().string();

        // Link state
        {
            std::ifstream f(base + "/operstate");
            std::string state;
            if (f && std::getline(f, state)) {
                nic->link_up = (state == "up");
            }
        }

        // Speed
        {
            std::ifstream f(base + "/speed");
            int speed_mbps = 0;
            if (f >> speed_mbps) {
                if (speed_mbps >= 1000) {
                    char buf[32]; std::snprintf(buf, sizeof(buf), "%dGb/s", speed_mbps / 1000);
                    nic->speed = buf;
                } else {
                    char buf[32]; std::snprintf(buf, sizeof(buf), "%dMb/s", speed_mbps);
                    nic->speed = buf;
                }
            }
        }

        // Driver
        {
            auto driver_link = base + "/device/driver";
            if (fs::is_symlink(driver_link)) {
                nic->driver = fs::read_symlink(driver_link).filename().string();
            }
        }

        // Statistics
        auto read_stat = [&](const std::string& stat_name) -> uint64_t {
            std::ifstream f(base + "/statistics/" + stat_name);
            uint64_t v = 0;
            if (f) f >> v;
            return v;
        };

        nic->rx_bytes = read_stat("rx_bytes");
        nic->tx_bytes = read_stat("tx_bytes");
        nic->rx_packets = read_stat("rx_packets");
        nic->tx_packets = read_stat("tx_packets");
        nic->rx_errors = read_stat("rx_errors");
        nic->tx_errors = read_stat("tx_errors");

        // RDMA detection
        nic->rdma_capable = fs::exists("/sys/class/infiniband") ||
                            (nic->driver == "mlx5_core" || nic->driver == "mlx4_core");

        // RDMA stats (if InfiniBand)
        if (nic->rdma_capable) {
            std::string ib_base = "/sys/class/infiniband";
            if (fs::exists(ib_base)) {
                for (auto& ib_entry : fs::directory_iterator(ib_base)) {
                    std::string port_dir = ib_entry.path().string() + "/ports/1/counters";
                    if (fs::exists(port_dir)) {
                        std::ifstream f1(port_dir + "/port_xmit_data");
                        if (f1) f1 >> nic->rdma_send_wrs;
                        std::ifstream f2(port_dir + "/port_rcv_data");
                        if (f2) f2 >> nic->rdma_recv_wrs;
                        break;
                    }
                }
            }
        }
    }
}

void NetworkTopologyWidget::compute_bandwidth() {
    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - last_bw_sample_).count();
    if (dt < 0.1f) return; // Too soon

    for (auto& nic : nics_) {
        if (nic.prev_rx_bytes > 0 && dt > 0) {
            uint64_t rx_delta = (nic.rx_bytes >= nic.prev_rx_bytes) ?
                                (nic.rx_bytes - nic.prev_rx_bytes) : 0;
            uint64_t tx_delta = (nic.tx_bytes >= nic.prev_tx_bytes) ?
                                (nic.tx_bytes - nic.prev_tx_bytes) : 0;
            nic.rx_bw_mbps = static_cast<float>(rx_delta) * 8.0f / (dt * 1e6f);
            nic.tx_bw_mbps = static_cast<float>(tx_delta) * 8.0f / (dt * 1e6f);
        }
        nic.prev_rx_bytes = nic.rx_bytes;
        nic.prev_tx_bytes = nic.tx_bytes;
    }
    last_bw_sample_ = now;
}

void NetworkTopologyWidget::push_history(NicInfo& nic) {
    int idx = nic.hist_offset % NicInfo::kHistLen;
    nic.rx_bw_hist[idx] = nic.rx_bw_mbps;
    nic.tx_bw_hist[idx] = nic.tx_bw_mbps;
    nic.hist_offset++;
}

void NetworkTopologyWidget::update() {
    if (!should_update()) return;
    read_sysfs_nics();
    compute_bandwidth();
    for (auto& nic : nics_) push_history(nic);
}

void NetworkTopologyWidget::render(bool* p_open) {
    if (!ImGui::Begin("Network Topology", p_open)) {
        ImGui::End();
        return;
    }

    if (nics_.empty()) {
        ImGui::TextWrapped("No network interfaces found in /sys/class/net.");
        ImGui::End();
        return;
    }

    // NIC table
    if (ImGui::BeginTable("##nic_table", 7,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable,
            ImVec2(0, 180))) {

        ImGui::TableSetupColumn("Interface");
        ImGui::TableSetupColumn("Driver");
        ImGui::TableSetupColumn("Speed");
        ImGui::TableSetupColumn("Link");
        ImGui::TableSetupColumn("RX BW");
        ImGui::TableSetupColumn("TX BW");
        ImGui::TableSetupColumn("RDMA");
        ImGui::TableHeadersRow();

        for (int i = 0; i < static_cast<int>(nics_.size()); ++i) {
            auto& nic = nics_[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable(nic.name.c_str(), selected_nic_ == i,
                    ImGuiSelectableFlags_SpanAllColumns)) {
                selected_nic_ = i;
            }
            ImGui::TableNextColumn(); ImGui::TextUnformatted(nic.driver.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(nic.speed.c_str());
            ImGui::TableNextColumn();
            ImGui::TextColored(nic.link_up ? ImVec4(0.3f, 1, 0.3f, 1) : ImVec4(1, 0.3f, 0.3f, 1),
                               nic.link_up ? "UP" : "DOWN");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(human_bw(nic.rx_bw_mbps).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(human_bw(nic.tx_bw_mbps).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(nic.rdma_capable ? "Yes" : "No");
        }
        ImGui::EndTable();
    }

    // Detail section
    if (selected_nic_ >= 0 && selected_nic_ < static_cast<int>(nics_.size())) {
        auto& nic = nics_[selected_nic_];
        ImGui::Separator();
        ImGui::Text("Interface: %s", nic.name.c_str());

        ImGui::Columns(2, nullptr, false);
        ImGui::Text("RX Bytes:   %s", human_bytes(nic.rx_bytes).c_str());
        ImGui::Text("RX Packets: %llu", static_cast<unsigned long long>(nic.rx_packets));
        ImGui::Text("RX Errors:  %llu", static_cast<unsigned long long>(nic.rx_errors));
        ImGui::NextColumn();
        ImGui::Text("TX Bytes:   %s", human_bytes(nic.tx_bytes).c_str());
        ImGui::Text("TX Packets: %llu", static_cast<unsigned long long>(nic.tx_packets));
        ImGui::Text("TX Errors:  %llu", static_cast<unsigned long long>(nic.tx_errors));
        ImGui::Columns(1);

        if (nic.rdma_capable) {
            ImGui::Separator();
            ImGui::Text("RDMA Send WRs: %llu", static_cast<unsigned long long>(nic.rdma_send_wrs));
            ImGui::Text("RDMA Recv WRs: %llu", static_cast<unsigned long long>(nic.rdma_recv_wrs));
        }

        // Bandwidth history
        int count = std::min(nic.hist_offset, NicInfo::kHistLen);
        if (count > 0) {
            ImGui::Separator();
            std::array<float, NicInfo::kHistLen> rx_plot{}, tx_plot{};
            float max_bw = 1.0f;
            for (int j = 0; j < count; ++j) {
                int src = (nic.hist_offset - count + j) % NicInfo::kHistLen;
                rx_plot[j] = nic.rx_bw_hist[src];
                tx_plot[j] = nic.tx_bw_hist[src];
                max_bw = std::max({max_bw, rx_plot[j], tx_plot[j]});
            }
            ImGui::Text("RX Bandwidth");
            ImGui::PlotLines("##rx_bw", rx_plot.data(), count, 0, nullptr, 0.0f, max_bw * 1.2f, ImVec2(-1, 50));
            ImGui::Text("TX Bandwidth");
            ImGui::PlotLines("##tx_bw", tx_plot.data(), count, 0, nullptr, 0.0f, max_bw * 1.2f, ImVec2(-1, 50));
        }
    }

    ImGui::End();
}

} // namespace straylight::widgets
