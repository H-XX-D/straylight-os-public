// apps/hub/network_panel.cpp
#include "network_panel.h"
#include <unordered_map>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include <dirent.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace straylight::hub {

void NetworkPanel::sample_interfaces() {
    // Read /proc/net/dev for traffic stats
    std::ifstream f("/proc/net/dev");
    if (!f.is_open()) return;

    // Save prev values
    std::unordered_map<std::string, InterfaceDetail*> prev_map;
    for (auto& iface : interfaces_) {
        prev_map[iface.name] = &iface;
    }

    std::vector<InterfaceDetail> new_ifaces;
    std::string line;
    int line_num = 0;
    while (std::getline(f, line)) {
        if (++line_num <= 2) continue;

        std::istringstream ss(line);
        std::string name;
        ss >> name;
        if (name.empty()) continue;
        if (name.back() == ':') name.pop_back();
        if (name == "lo") continue;

        InterfaceDetail iface;
        iface.name = name;
        ss >> iface.rx_bytes >> iface.rx_packets >> iface.rx_errors >> iface.rx_dropped;
        uint64_t dummy;
        ss >> dummy >> dummy >> dummy >> dummy;
        ss >> iface.tx_bytes >> iface.tx_packets >> iface.tx_errors >> iface.tx_dropped;

        // Copy history from previous sample
        auto it = prev_map.find(name);
        if (it != prev_map.end()) {
            auto* prev = it->second;
            iface.prev_rx_bytes = prev->rx_bytes;
            iface.prev_tx_bytes = prev->tx_bytes;

            uint64_t rx_diff = (iface.rx_bytes >= iface.prev_rx_bytes) ?
                iface.rx_bytes - iface.prev_rx_bytes : 0;
            uint64_t tx_diff = (iface.tx_bytes >= iface.prev_tx_bytes) ?
                iface.tx_bytes - iface.prev_tx_bytes : 0;

            iface.rx_rate_mbps = static_cast<float>(rx_diff) / (1024.0f * 1024.0f);
            iface.tx_rate_mbps = static_cast<float>(tx_diff) / (1024.0f * 1024.0f);

            // Copy history
            std::memcpy(iface.rx_history, prev->rx_history, sizeof(iface.rx_history));
            std::memcpy(iface.tx_history, prev->tx_history, sizeof(iface.tx_history));
            iface.history_idx = prev->history_idx;
        }

        // Update history
        int idx = iface.history_idx % InterfaceDetail::HISTORY_LEN;
        iface.rx_history[idx] = iface.rx_rate_mbps;
        iface.tx_history[idx] = iface.tx_rate_mbps;
        iface.history_idx++;

        // Interface state
        std::string state_path = "/sys/class/net/" + name + "/operstate";
        std::ifstream state_f(state_path);
        if (state_f.is_open()) {
            std::string state;
            state_f >> state;
            iface.up = (state == "up");
        }

        // MTU
        std::string mtu_path = "/sys/class/net/" + name + "/mtu";
        std::ifstream mtu_f(mtu_path);
        if (mtu_f.is_open()) {
            mtu_f >> iface.mtu;
        }

        // Speed
        std::string speed_path = "/sys/class/net/" + name + "/speed";
        std::ifstream speed_f(speed_path);
        if (speed_f.is_open()) {
            int spd = 0;
            speed_f >> spd;
            if (spd > 0) {
                iface.speed = std::to_string(spd) + " Mb/s";
            }
        }

        // MAC address
        std::string mac_path = "/sys/class/net/" + name + "/address";
        std::ifstream mac_f(mac_path);
        if (mac_f.is_open()) {
            std::getline(mac_f, iface.mac);
        }

        new_ifaces.push_back(iface);
    }

    // Get IP addresses via getifaddrs
    struct ifaddrs* ifas = nullptr;
    if (::getifaddrs(&ifas) == 0) {
        for (struct ifaddrs* ifa = ifas; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr) continue;
            std::string ifname = ifa->ifa_name;
            if (ifname == "lo") continue;

            for (auto& iface : new_ifaces) {
                if (iface.name != ifname) continue;

                if (ifa->ifa_addr->sa_family == AF_INET) {
                    auto* addr = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
                    char buf[INET_ADDRSTRLEN];
                    ::inet_ntop(AF_INET, &addr->sin_addr, buf, sizeof(buf));
                    iface.ipv4 = buf;
                } else if (ifa->ifa_addr->sa_family == AF_INET6) {
                    auto* addr = reinterpret_cast<struct sockaddr_in6*>(ifa->ifa_addr);
                    char buf[INET6_ADDRSTRLEN];
                    ::inet_ntop(AF_INET6, &addr->sin6_addr, buf, sizeof(buf));
                    iface.ipv6 = buf;
                }
            }
        }
        ::freeifaddrs(ifas);
    }

    interfaces_ = std::move(new_ifaces);
}

void NetworkPanel::sample_connections() {
    connections_.clear();

    // Parse /proc/net/tcp and /proc/net/tcp6
    for (const auto& proto_file : {
        std::pair{"tcp", "/proc/net/tcp"},
        std::pair{"tcp6", "/proc/net/tcp6"},
        std::pair{"udp", "/proc/net/udp"},
        std::pair{"udp6", "/proc/net/udp6"}
    }) {
        std::ifstream f(proto_file.second);
        if (!f.is_open()) continue;

        std::string line;
        bool first = true;
        while (std::getline(f, line)) {
            if (first) { first = false; continue; } // Skip header

            std::istringstream ss(line);
            int sl;
            std::string local_hex, remote_hex;
            int state_int;

            ss >> sl >> local_hex >> remote_hex >> std::hex >> state_int;

            NetConnection conn;
            conn.protocol = proto_file.first;

            // Parse hex addresses (simplified for IPv4)
            auto parse_addr = [](const std::string& hex_addr) -> std::string {
                auto colon_pos = hex_addr.find(':');
                if (colon_pos == std::string::npos) return hex_addr;

                std::string ip_hex = hex_addr.substr(0, colon_pos);
                std::string port_hex = hex_addr.substr(colon_pos + 1);

                // Parse IPv4 address (stored in little-endian hex)
                unsigned int ip_int = 0;
                try { ip_int = std::stoul(ip_hex, nullptr, 16); } catch (...) {}

                unsigned int port = 0;
                try { port = std::stoul(port_hex, nullptr, 16); } catch (...) {}

                char buf[32];
                std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u",
                              ip_int & 0xFF,
                              (ip_int >> 8) & 0xFF,
                              (ip_int >> 16) & 0xFF,
                              (ip_int >> 24) & 0xFF,
                              port);
                return buf;
            };

            conn.local_addr = parse_addr(local_hex);
            conn.remote_addr = parse_addr(remote_hex);

            // TCP states
            static const char* tcp_states[] = {
                "", "ESTABLISHED", "SYN_SENT", "SYN_RECV", "FIN_WAIT1",
                "FIN_WAIT2", "TIME_WAIT", "CLOSE", "CLOSE_WAIT",
                "LAST_ACK", "LISTEN", "CLOSING"
            };
            if (state_int >= 0 && state_int <= 11) {
                conn.state = tcp_states[state_int];
            } else {
                conn.state = std::to_string(state_int);
            }

            connections_.push_back(conn);
        }
    }
}

void NetworkPanel::sample() {
    sample_interfaces();
    sample_connections();
    first_sample_ = false;
}

void NetworkPanel::render_interface(InterfaceDetail& iface) {
    ImGui::PushID(iface.name.c_str());

    ImVec4 state_color = iface.up ? ImVec4(0.0f, 0.9f, 0.6f, 1.0f)
                                  : ImVec4(1.0f, 0.3f, 0.3f, 1.0f);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.08f, 0.14f, 1.0f));
    ImGui::BeginChild(iface.name.c_str(), ImVec2(0, 160), true);

    ImGui::TextColored(state_color, "%s", iface.up ? "[UP]" : "[DOWN]");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.0f, 0.9f, 0.6f, 1.0f), "%s", iface.name.c_str());
    ImGui::SameLine(200.0f);
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s", iface.mac.c_str());
    ImGui::SameLine(380.0f);
    if (!iface.speed.empty()) {
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s", iface.speed.c_str());
    }

    // IP addresses
    if (!iface.ipv4.empty()) {
        ImGui::Text("  IPv4: %s", iface.ipv4.c_str());
    }
    if (!iface.ipv6.empty()) {
        ImGui::SameLine(300.0f);
        ImGui::Text("IPv6: %s", iface.ipv6.c_str());
    }

    // Traffic stats
    ImGui::Text("  RX: %.2f MB/s (%llu pkts, %llu err)",
                iface.rx_rate_mbps,
                static_cast<unsigned long long>(iface.rx_packets),
                static_cast<unsigned long long>(iface.rx_errors));
    ImGui::SameLine(400.0f);
    ImGui::Text("TX: %.2f MB/s (%llu pkts, %llu err)",
                iface.tx_rate_mbps,
                static_cast<unsigned long long>(iface.tx_packets),
                static_cast<unsigned long long>(iface.tx_errors));

    // Traffic graphs
    int len = std::min(iface.history_idx, InterfaceDetail::HISTORY_LEN);
    int offset = iface.history_idx % InterfaceDetail::HISTORY_LEN;

    float ordered_rx[InterfaceDetail::HISTORY_LEN]{};
    float ordered_tx[InterfaceDetail::HISTORY_LEN]{};
    float max_val = 0.1f;
    for (int i = 0; i < len; ++i) {
        int src = (offset - len + i + InterfaceDetail::HISTORY_LEN) % InterfaceDetail::HISTORY_LEN;
        ordered_rx[i] = iface.rx_history[src];
        ordered_tx[i] = iface.tx_history[src];
        if (ordered_rx[i] > max_val) max_val = ordered_rx[i];
        if (ordered_tx[i] > max_val) max_val = ordered_tx[i];
    }

    ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(0.0f, 0.9f, 0.6f, 1.0f));
    ImGui::PlotLines("##rx", ordered_rx, len, 0, "RX", 0.0f, max_val * 1.2f, ImVec2(0, 30));
    ImGui::PopStyleColor();

    ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(0.4f, 0.6f, 1.0f, 1.0f));
    ImGui::PlotLines("##tx", ordered_tx, len, 0, "TX", 0.0f, max_val * 1.2f, ImVec2(0, 30));
    ImGui::PopStyleColor();

    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopID();
}

void NetworkPanel::render_connections_table() {
    // Filter
    ImGui::InputText("Filter", filter_buf_, sizeof(filter_buf_));
    std::string filter(filter_buf_);

    ImGui::Separator();

    // Table headers
    ImGui::Columns(5, "conn_table", true);
    ImGui::SetColumnWidth(0, 70.0f);
    ImGui::SetColumnWidth(1, 200.0f);
    ImGui::SetColumnWidth(2, 200.0f);
    ImGui::SetColumnWidth(3, 120.0f);
    ImGui::SetColumnWidth(4, 100.0f);

    ImGui::Text("Proto"); ImGui::NextColumn();
    ImGui::Text("Local Address"); ImGui::NextColumn();
    ImGui::Text("Remote Address"); ImGui::NextColumn();
    ImGui::Text("State"); ImGui::NextColumn();
    ImGui::Text("PID"); ImGui::NextColumn();
    ImGui::Separator();

    int shown = 0;
    for (const auto& conn : connections_) {
        // Apply filter
        if (!filter.empty()) {
            bool matches = conn.protocol.find(filter) != std::string::npos ||
                          conn.local_addr.find(filter) != std::string::npos ||
                          conn.remote_addr.find(filter) != std::string::npos ||
                          conn.state.find(filter) != std::string::npos;
            if (!matches) continue;
        }

        if (++shown > 200) {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "...");
            break;
        }

        // Color by state
        ImVec4 state_color;
        if (conn.state == "ESTABLISHED") state_color = ImVec4(0.0f, 0.9f, 0.6f, 1.0f);
        else if (conn.state == "LISTEN") state_color = ImVec4(0.4f, 0.6f, 1.0f, 1.0f);
        else if (conn.state == "TIME_WAIT") state_color = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
        else if (conn.state == "CLOSE_WAIT") state_color = ImVec4(1.0f, 0.8f, 0.2f, 1.0f);
        else state_color = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);

        ImGui::Text("%s", conn.protocol.c_str()); ImGui::NextColumn();
        ImGui::Text("%s", conn.local_addr.c_str()); ImGui::NextColumn();
        ImGui::Text("%s", conn.remote_addr.c_str()); ImGui::NextColumn();
        ImGui::TextColored(state_color, "%s", conn.state.c_str()); ImGui::NextColumn();
        if (conn.pid > 0) {
            ImGui::Text("%d", conn.pid);
        } else {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "-");
        }
        ImGui::NextColumn();
    }

    ImGui::Columns(1);

    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                       "%zu connections total, %d shown",
                       connections_.size(), shown);
}

void NetworkPanel::render() {
    // Interface cards
    if (ImGui::CollapsingHeader("Interfaces", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (auto& iface : interfaces_) {
            render_interface(iface);
            ImGui::Spacing();
        }
    }

    // Active connections table
    if (ImGui::CollapsingHeader("Active Connections", ImGuiTreeNodeFlags_DefaultOpen)) {
        render_connections_table();
    }
}

} // namespace straylight::hub
