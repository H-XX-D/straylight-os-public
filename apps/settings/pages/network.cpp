// apps/settings/pages/network.cpp
// Network settings — WiFi scanning via NetworkManager D-Bus, connection management
#include "network.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace straylight::settings {

namespace fs = std::filesystem;

NetworkPage::NetworkPage() = default;

void NetworkPage::load() {
    read_connections();
    scan_wifi();
}

void NetworkPage::read_connections() {
    connections_.clear();

    // Read network interfaces from sysfs
    std::error_code ec;
    fs::path net_dir = "/sys/class/net";
    if (!fs::exists(net_dir, ec)) return;

    for (const auto& entry : fs::directory_iterator(net_dir, ec)) {
        std::string iface = entry.path().filename().string();
        if (iface == "lo") continue;

        ConnectionInfo conn;
        conn.interface_name = iface;

        // Determine type
        std::ifstream type_file(entry.path() / "type");
        if (type_file.is_open()) {
            int type_num = 0;
            type_file >> type_num;
            if (type_num == 1) {
                // Check if wireless
                if (fs::exists(entry.path() / "wireless", ec)) {
                    conn.type = "wifi";
                } else {
                    conn.type = "ethernet";
                }
            }
        }

        // Check operstate
        std::ifstream oper_file(entry.path() / "operstate");
        if (oper_file.is_open()) {
            std::getline(oper_file, conn.state);
            if (conn.state == "up") conn.state = "connected";
            else if (conn.state == "down") conn.state = "disconnected";
        }

        // MAC address
        std::ifstream mac_file(entry.path() / "address");
        if (mac_file.is_open()) {
            std::getline(mac_file, conn.mac_address);
        }

        // Speed (for ethernet)
        std::ifstream speed_file(entry.path() / "speed");
        if (speed_file.is_open()) {
            speed_file >> conn.speed_mbps;
            if (conn.speed_mbps < 0) conn.speed_mbps = 0;
        }

        // IP address via ip command
        char cmd[256];
        snprintf(cmd, sizeof(cmd),
                 "ip -4 addr show %s 2>/dev/null | grep -oP 'inet \\K[\\d.]+/[\\d]+'",
                 iface.c_str());
        FILE* pipe = popen(cmd, "r");
        if (pipe) {
            char buf[64];
            if (fgets(buf, sizeof(buf), pipe)) {
                conn.ip4_address = buf;
                // Remove trailing newline
                while (!conn.ip4_address.empty() &&
                       (conn.ip4_address.back() == '\n' ||
                        conn.ip4_address.back() == '\r')) {
                    conn.ip4_address.pop_back();
                }
            }
            pclose(pipe);
        }

        // Default gateway
        snprintf(cmd, sizeof(cmd),
                 "ip route show default dev %s 2>/dev/null | grep -oP 'via \\K[\\d.]+'",
                 iface.c_str());
        pipe = popen(cmd, "r");
        if (pipe) {
            char buf[64];
            if (fgets(buf, sizeof(buf), pipe)) {
                conn.ip4_gateway = buf;
                while (!conn.ip4_gateway.empty() &&
                       (conn.ip4_gateway.back() == '\n' ||
                        conn.ip4_gateway.back() == '\r')) {
                    conn.ip4_gateway.pop_back();
                }
            }
            pclose(pipe);
        }

        // DNS
        std::ifstream resolv("/etc/resolv.conf");
        if (resolv.is_open()) {
            std::string line;
            while (std::getline(resolv, line)) {
                if (line.compare(0, 11, "nameserver ") == 0) {
                    conn.ip4_dns = line.substr(11);
                    break;
                }
            }
        }

        connections_.push_back(std::move(conn));
    }
}

Result<void, std::string> NetworkPage::scan_wifi() {
    wifi_networks_.clear();

    // Use nmcli to scan WiFi networks
    FILE* pipe = popen(
        "nmcli -t -f SSID,BSSID,SIGNAL,FREQ,SECURITY,IN-USE "
        "device wifi list 2>/dev/null",
        "r");

    if (!pipe) {
        // Fall back to iw scan
        pipe = popen(
            "iw dev wlan0 scan 2>/dev/null | "
            "grep -E '(SSID:|signal:|freq:)' 2>/dev/null",
            "r");

        if (!pipe) {
            return Result<void, std::string>::error(
                "Neither nmcli nor iw available for WiFi scanning");
        }

        // Parse iw output
        char buf[512];
        WifiNetwork current;
        while (fgets(buf, sizeof(buf), pipe)) {
            std::string line(buf);
            // Trim
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
                line.pop_back();
            while (!line.empty() && line[0] == '\t')
                line.erase(line.begin());

            if (line.compare(0, 5, "SSID:") == 0) {
                if (!current.ssid.empty()) {
                    wifi_networks_.push_back(current);
                }
                current = {};
                current.ssid = line.substr(6);
            } else if (line.compare(0, 7, "signal:") == 0) {
                sscanf(line.c_str(), "signal: %d", &current.signal_strength);
                // Convert dBm to percentage roughly
                current.signal_strength = std::clamp(
                    (current.signal_strength + 100) * 2, 0, 100);
            } else if (line.compare(0, 5, "freq:") == 0) {
                sscanf(line.c_str(), "freq: %d", &current.frequency_mhz);
            }
        }
        if (!current.ssid.empty()) {
            wifi_networks_.push_back(current);
        }
        pclose(pipe);

        return Result<void, std::string>::ok();
    }

    // Parse nmcli output
    char buf[512];
    while (fgets(buf, sizeof(buf), pipe)) {
        std::string line(buf);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();

        // Format: SSID:BSSID:SIGNAL:FREQ:SECURITY:IN-USE
        WifiNetwork net;
        std::istringstream ss(line);
        std::string field;

        int idx = 0;
        while (std::getline(ss, field, ':')) {
            switch (idx) {
            case 0: net.ssid = field; break;
            case 1: net.bssid = field; break;
            case 2:
                try { net.signal_strength = std::stoi(field); }
                catch (...) {}
                break;
            case 3:
                try { net.frequency_mhz = std::stoi(field); }
                catch (...) {}
                break;
            case 4: net.security = field; break;
            case 5: net.connected = (field == "*"); break;
            }
            idx++;
        }

        if (!net.ssid.empty()) {
            wifi_networks_.push_back(std::move(net));
        }
    }
    pclose(pipe);

    // Sort by signal strength
    std::sort(wifi_networks_.begin(), wifi_networks_.end(),
              [](const WifiNetwork& a, const WifiNetwork& b) {
                  return a.signal_strength > b.signal_strength;
              });

    return Result<void, std::string>::ok();
}

Result<void, std::string> NetworkPage::connect(const std::string& ssid,
                                                 const std::string& password) {
    char cmd[512];
    if (password.empty()) {
        snprintf(cmd, sizeof(cmd),
                 "nmcli device wifi connect '%s' 2>&1",
                 ssid.c_str());
    } else {
        snprintf(cmd, sizeof(cmd),
                 "nmcli device wifi connect '%s' password '%s' 2>&1",
                 ssid.c_str(), password.c_str());
    }

    FILE* pipe = popen(cmd, "r");
    if (!pipe) {
        return Result<void, std::string>::error("Failed to execute nmcli");
    }

    std::string output;
    char buf[256];
    while (fgets(buf, sizeof(buf), pipe)) {
        output += buf;
    }
    int status = pclose(pipe);

    if (status != 0) {
        return Result<void, std::string>::error("Connection failed: " + output);
    }

    // Refresh
    read_connections();

    return Result<void, std::string>::ok();
}

Result<void, std::string> NetworkPage::disconnect() {
    FILE* pipe = popen("nmcli device disconnect wlan0 2>&1", "r");
    if (!pipe) {
        return Result<void, std::string>::error("Failed to execute nmcli");
    }

    std::string output;
    char buf[256];
    while (fgets(buf, sizeof(buf), pipe)) {
        output += buf;
    }
    int status = pclose(pipe);

    if (status != 0) {
        return Result<void, std::string>::error("Disconnect failed: " + output);
    }

    read_connections();
    return Result<void, std::string>::ok();
}

void NetworkPage::render() {
    ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.67f, 1.0f), "Network Settings");
    ImGui::Separator();

    // === Active Connections ===
    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Connections");

    for (const auto& conn : connections_) {
        ImGui::PushID(conn.interface_name.c_str());

        bool is_connected = (conn.state == "connected");
        ImVec4 state_color = is_connected
                                 ? ImVec4(0.0f, 1.0f, 0.5f, 1.0f)
                                 : ImVec4(0.5f, 0.5f, 0.5f, 1.0f);

        if (ImGui::CollapsingHeader(
                (conn.interface_name + " (" + conn.type + ")").c_str(),
                ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextColored(state_color, "Status: %s", conn.state.c_str());
            ImGui::Text("MAC: %s", conn.mac_address.c_str());

            if (!conn.ip4_address.empty()) {
                ImGui::Text("IPv4: %s", conn.ip4_address.c_str());
            }
            if (!conn.ip4_gateway.empty()) {
                ImGui::Text("Gateway: %s", conn.ip4_gateway.c_str());
            }
            if (!conn.ip4_dns.empty()) {
                ImGui::Text("DNS: %s", conn.ip4_dns.c_str());
            }
            if (conn.speed_mbps > 0) {
                ImGui::Text("Speed: %d Mbps", conn.speed_mbps);
            }

            if (is_connected && conn.type == "wifi") {
                if (ImGui::Button("Disconnect")) {
                    disconnect();
                }
            }
        }

        ImGui::PopID();
    }

    ImGui::Spacing();
    ImGui::Separator();

    // === WiFi Networks ===
    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "WiFi Networks");

    if (ImGui::Button("Scan")) {
        scan_wifi();
    }

    ImGui::Spacing();

    for (int i = 0; i < static_cast<int>(wifi_networks_.size()); ++i) {
        const auto& net = wifi_networks_[static_cast<size_t>(i)];
        ImGui::PushID(i);

        // Signal strength indicator
        const char* signal_icon;
        ImVec4 signal_color;
        if (net.signal_strength >= 75) {
            signal_icon = "[||||]";
            signal_color = ImVec4(0.0f, 1.0f, 0.5f, 1.0f);
        } else if (net.signal_strength >= 50) {
            signal_icon = "[||| ]";
            signal_color = ImVec4(0.8f, 0.8f, 0.0f, 1.0f);
        } else if (net.signal_strength >= 25) {
            signal_icon = "[||  ]";
            signal_color = ImVec4(1.0f, 0.5f, 0.0f, 1.0f);
        } else {
            signal_icon = "[|   ]";
            signal_color = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
        }

        ImGui::TextColored(signal_color, "%s", signal_icon);
        ImGui::SameLine();

        bool selected = (i == selected_wifi_);
        std::string label = net.ssid;
        if (net.connected) label += " (Connected)";
        if (!net.security.empty() && net.security != "--") {
            label += " [" + net.security + "]";
        }

        if (ImGui::Selectable(label.c_str(), selected)) {
            selected_wifi_ = i;
        }

        // Double-click to connect
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            if (!net.connected) {
                if (net.security.empty() || net.security == "--" ||
                    net.security == "open") {
                    connect(net.ssid, "");
                } else {
                    show_password_dialog_ = true;
                }
            }
        }

        ImGui::SameLine(ImGui::GetWindowWidth() - 150);
        ImGui::TextDisabled("%d%% | %d MHz",
                            net.signal_strength, net.frequency_mhz);

        ImGui::PopID();
    }

    // Connect button for selected network
    if (selected_wifi_ >= 0 &&
        selected_wifi_ < static_cast<int>(wifi_networks_.size())) {
        const auto& sel = wifi_networks_[static_cast<size_t>(selected_wifi_)];
        if (!sel.connected) {
            ImGui::Spacing();
            if (ImGui::Button("Connect", ImVec2(120, 30))) {
                if (sel.security.empty() || sel.security == "--" ||
                    sel.security == "open") {
                    connect(sel.ssid, "");
                } else {
                    show_password_dialog_ = true;
                }
            }
        }
    }

    // Password dialog
    if (show_password_dialog_) {
        ImGui::OpenPopup("WiFi Password");
        show_password_dialog_ = false;
    }

    if (ImGui::BeginPopupModal("WiFi Password", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        static char pw_buf[128] = {};

        if (selected_wifi_ >= 0 &&
            selected_wifi_ < static_cast<int>(wifi_networks_.size())) {
            ImGui::Text("Connect to: %s",
                        wifi_networks_[static_cast<size_t>(selected_wifi_)]
                            .ssid.c_str());
        }
        ImGui::InputText("Password", pw_buf, sizeof(pw_buf),
                          ImGuiInputTextFlags_Password);

        if (ImGui::Button("Connect", ImVec2(120, 0))) {
            if (selected_wifi_ >= 0) {
                connect(
                    wifi_networks_[static_cast<size_t>(selected_wifi_)].ssid,
                    pw_buf);
            }
            pw_buf[0] = '\0';
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            pw_buf[0] = '\0';
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

} // namespace straylight::settings
