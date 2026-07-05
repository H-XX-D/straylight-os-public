// apps/oobe/pages/network_config.h
// OOBE network configuration page
#pragma once

#include <string>
#include <vector>

namespace straylight::oobe {

/// Represents a detected network connection.
struct NetworkEntry {
    std::string name;       // SSID or interface name
    std::string type;       // "wifi" or "ethernet"
    int         rssi = 0;   // Signal strength in dBm (wifi only)
    bool        connected = false;
};

/// Convert RSSI (dBm) to signal bars (1-4).
int rssi_to_bars(int rssi);

/// Network configuration page — Ethernet and WiFi setup.
class NetworkConfigPage {
public:
    NetworkConfigPage() = default;
    ~NetworkConfigPage() = default;

    /// Render the page. Returns true to advance.
    bool render();

    /// Scan for available networks (via NetworkManager D-Bus).
    /// Returns empty list if D-Bus is unavailable.
    std::vector<NetworkEntry> scan();

private:
    std::vector<NetworkEntry> networks_;
    int selected_ = -1;
    char passphrase_buf_[128] = {};
    std::string status_message_;
    bool scanned_ = false;
};

} // namespace straylight::oobe
