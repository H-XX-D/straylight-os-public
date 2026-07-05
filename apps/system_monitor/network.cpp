// apps/system_monitor/network.cpp
// Network monitoring via /proc/net/dev
#include "network.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace straylight::sysmon {

std::string format_bandwidth(double bps) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1);
    if (bps < 1024.0) {
        ss << bps << " B/s";
    } else if (bps < 1024.0 * 1024.0) {
        ss << bps / 1024.0 << " KB/s";
    } else if (bps < 1024.0 * 1024.0 * 1024.0) {
        ss << bps / (1024.0 * 1024.0) << " MB/s";
    } else {
        ss << bps / (1024.0 * 1024.0 * 1024.0) << " GB/s";
    }
    return ss.str();
}

std::string format_bytes(uint64_t bytes) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1);
    double b = static_cast<double>(bytes);
    if (b < 1024.0) {
        ss << bytes << " B";
    } else if (b < 1024.0 * 1024.0) {
        ss << b / 1024.0 << " KB";
    } else if (b < 1024.0 * 1024.0 * 1024.0) {
        ss << b / (1024.0 * 1024.0) << " MB";
    } else {
        ss << b / (1024.0 * 1024.0 * 1024.0) << " GB";
    }
    return ss.str();
}

NetworkMonitor::NetworkMonitor() = default;

double NetworkMonitor::get_time_seconds() const {
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) +
           static_cast<double>(ts.tv_nsec) / 1e9;
}

Result<void, std::string> NetworkMonitor::sample() {
    std::ifstream file("/proc/net/dev");
    if (!file.is_open()) {
        return Result<void, std::string>::error("Cannot open /proc/net/dev");
    }

    double now = get_time_seconds();
    double dt = first_sample_ ? 1.0 : (now - last_sample_time_);
    if (dt <= 0.0) dt = 1.0;

    std::vector<InterfaceStats> current;

    std::string line;
    // Skip header lines
    std::getline(file, line);
    std::getline(file, line);

    while (std::getline(file, line)) {
        InterfaceStats iface;

        // Parse: "  iface: rx_bytes rx_packets rx_errors rx_dropped ..."
        char name[64] = {};
        uint64_t rx_bytes, rx_packets, rx_errors, rx_dropped;
        uint64_t rx_fifo, rx_frame, rx_compressed, rx_multicast;
        uint64_t tx_bytes, tx_packets, tx_errors, tx_dropped;
        uint64_t tx_fifo, tx_colls, tx_carrier, tx_compressed;

        int scanned = sscanf(line.c_str(),
            " %63[^:]: %lu %lu %lu %lu %lu %lu %lu %lu "
            "%lu %lu %lu %lu %lu %lu %lu %lu",
            name,
            &rx_bytes, &rx_packets, &rx_errors, &rx_dropped,
            &rx_fifo, &rx_frame, &rx_compressed, &rx_multicast,
            &tx_bytes, &tx_packets, &tx_errors, &tx_dropped,
            &tx_fifo, &tx_colls, &tx_carrier, &tx_compressed);

        if (scanned < 13) continue;

        iface.name = name;
        // Trim whitespace from name
        while (!iface.name.empty() && iface.name[0] == ' ') {
            iface.name.erase(iface.name.begin());
        }

        iface.rx_bytes = rx_bytes;
        iface.tx_bytes = tx_bytes;
        iface.rx_packets = rx_packets;
        iface.tx_packets = tx_packets;
        iface.rx_errors = rx_errors;
        iface.tx_errors = tx_errors;
        iface.rx_dropped = rx_dropped;
        iface.tx_dropped = tx_dropped;

        // Calculate bandwidth from previous sample
        if (!first_sample_) {
            for (const auto& prev : prev_interfaces_) {
                if (prev.name == iface.name) {
                    uint64_t rx_diff = rx_bytes >= prev.rx_bytes
                                           ? rx_bytes - prev.rx_bytes
                                           : 0;
                    uint64_t tx_diff = tx_bytes >= prev.tx_bytes
                                           ? tx_bytes - prev.tx_bytes
                                           : 0;

                    iface.rx_bps = static_cast<double>(rx_diff) / dt;
                    iface.tx_bps = static_cast<double>(tx_diff) / dt;

                    // Copy and extend history
                    iface.rx_history = prev.rx_history;
                    iface.tx_history = prev.tx_history;
                    break;
                }
            }
        }

        iface.rx_history.push_back(
            static_cast<float>(iface.rx_bps / 1024.0)); // KB/s
        while (static_cast<int>(iface.rx_history.size()) > InterfaceStats::kMaxHistory) {
            iface.rx_history.pop_front();
        }

        iface.tx_history.push_back(
            static_cast<float>(iface.tx_bps / 1024.0));
        while (static_cast<int>(iface.tx_history.size()) > InterfaceStats::kMaxHistory) {
            iface.tx_history.pop_front();
        }

        current.push_back(std::move(iface));
    }

    prev_interfaces_ = interfaces_;
    interfaces_ = std::move(current);
    last_sample_time_ = now;
    first_sample_ = false;

    return Result<void, std::string>::ok();
}

void NetworkMonitor::render() {
    ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.67f, 1.0f), "Network");
    ImGui::Spacing();

    if (interfaces_.empty()) {
        ImGui::TextDisabled("No network interfaces found");
        return;
    }

    for (const auto& iface : interfaces_) {
        // Skip loopback unless it has traffic
        if (iface.name == "lo" && iface.rx_bytes == 0 && iface.tx_bytes == 0) {
            continue;
        }

        ImGui::PushID(iface.name.c_str());

        if (ImGui::CollapsingHeader(iface.name.c_str(),
                                     ImGuiTreeNodeFlags_DefaultOpen)) {
            // Bandwidth
            ImGui::Columns(2, "netStats", false);
            ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f),
                              "RX: %s", format_bandwidth(iface.rx_bps).c_str());
            ImGui::NextColumn();
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f),
                              "TX: %s", format_bandwidth(iface.tx_bps).c_str());
            ImGui::NextColumn();

            ImGui::Text("Total RX: %s", format_bytes(iface.rx_bytes).c_str());
            ImGui::NextColumn();
            ImGui::Text("Total TX: %s", format_bytes(iface.tx_bytes).c_str());
            ImGui::NextColumn();

            ImGui::Text("Packets: %lu", iface.rx_packets);
            ImGui::NextColumn();
            ImGui::Text("Packets: %lu", iface.tx_packets);
            ImGui::NextColumn();

            ImGui::Text("Errors: %lu  Dropped: %lu",
                        iface.rx_errors, iface.rx_dropped);
            ImGui::NextColumn();
            ImGui::Text("Errors: %lu  Dropped: %lu",
                        iface.tx_errors, iface.tx_dropped);
            ImGui::NextColumn();
            ImGui::Columns(1);

            // RX graph
            if (!iface.rx_history.empty()) {
                std::vector<float> rxh(iface.rx_history.begin(),
                                        iface.rx_history.end());
                float max_val = *std::max_element(rxh.begin(), rxh.end());
                max_val = std::max(max_val, 1.0f);

                ImGui::PushStyleColor(ImGuiCol_PlotLines,
                                      IM_COL32(77, 204, 255, 255));
                ImGui::PlotLines("##RX", rxh.data(),
                                 static_cast<int>(rxh.size()),
                                 0, "RX KB/s", 0.0f, max_val * 1.2f,
                                 ImVec2(0, 50));
                ImGui::PopStyleColor();
            }

            // TX graph
            if (!iface.tx_history.empty()) {
                std::vector<float> txh(iface.tx_history.begin(),
                                        iface.tx_history.end());
                float max_val = *std::max_element(txh.begin(), txh.end());
                max_val = std::max(max_val, 1.0f);

                ImGui::PushStyleColor(ImGuiCol_PlotLines,
                                      IM_COL32(255, 128, 77, 255));
                ImGui::PlotLines("##TX", txh.data(),
                                 static_cast<int>(txh.size()),
                                 0, "TX KB/s", 0.0f, max_val * 1.2f,
                                 ImVec2(0, 50));
                ImGui::PopStyleColor();
            }
        }

        ImGui::PopID();
        ImGui::Spacing();
    }
}

} // namespace straylight::sysmon
