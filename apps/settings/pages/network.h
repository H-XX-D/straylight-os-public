// apps/settings/pages/network.h
// Network settings — WiFi scanning, connect/disconnect, IP configuration
#pragma once

#include "../settings_page.h"

#include <straylight/result.h>

#include <cstdint>
#include <string>
#include <vector>

namespace straylight::settings {

/// WiFi network info from NetworkManager D-Bus.
struct WifiNetwork {
    std::string ssid;
    std::string bssid;
    int signal_strength = 0; // 0..100
    int frequency_mhz = 0;
    std::string security; // "open", "wpa", "wpa2", "wpa3"
    bool connected = false;
    bool saved = false;
};

/// Connection info.
struct ConnectionInfo {
    std::string interface_name;
    std::string type; // "wifi", "ethernet", "vpn"
    std::string state; // "connected", "disconnected", etc.
    std::string ip4_address;
    std::string ip4_gateway;
    std::string ip4_dns;
    std::string ip6_address;
    std::string mac_address;
    int speed_mbps = 0;
};

/// Network settings page.
class NetworkPage : public SettingsPage {
public:
    NetworkPage();

    [[nodiscard]] const char* label() const override { return "Network"; }

    /// Load current network state.
    void load() override;

    /// Render the network settings page in ImGui.
    void render() override;

    /// Scan for WiFi networks.
    Result<void, std::string> scan_wifi();

    /// Connect to a WiFi network.
    Result<void, std::string> connect(const std::string& ssid,
                                       const std::string& password);

    /// Disconnect from the current network.
    Result<void, std::string> disconnect();

private:
    void read_connections();
    void read_wifi_networks();

    std::vector<ConnectionInfo> connections_;
    std::vector<WifiNetwork> wifi_networks_;
    bool scanning_ = false;
    int selected_wifi_ = -1;
    bool show_password_dialog_ = false;
};

} // namespace straylight::settings
