// apps/oobe/pages/network_config.cpp
// Network configuration page implementation
#include "network_config.h"

#include <straylight/log.h>

#include <imgui.h>

#include <algorithm>
#include <set>

namespace straylight::oobe {

int rssi_to_bars(int rssi) {
    // RSSI ranges: > -50 = 4 bars, > -60 = 3, > -70 = 2, else 1
    if (rssi > -50) return 4;
    if (rssi > -60) return 3;
    if (rssi > -70) return 2;
    return 1;
}

std::vector<NetworkEntry> NetworkConfigPage::scan() {
    // TODO: Use sdbus-c++ to call org.freedesktop.NetworkManager
    //       GetAllAccessPoints on WiFi device, GetDevices for Ethernet.
    //       Deduplicate SSIDs (same SSID on 2.4/5GHz shows once, best RSSI).

    SL_INFO("Network scan requested (D-Bus integration pending)");

    // Return empty list — D-Bus not available on build host
    return {};
}

bool NetworkConfigPage::render() {
    const ImGuiIO& io = ImGui::GetIO();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse;

    ImGui::Begin("##OobeNetwork", nullptr, flags);

    ImGui::SetCursorPosY(60.0f);
    ImGui::SetCursorPosX(60.0f);
    ImGui::Text("Network Configuration");
    ImGui::Separator();
    ImGui::Spacing();

    // Scan button
    ImGui::SetCursorPosX(60.0f);
    if (ImGui::Button("Scan for Networks", ImVec2(180, 32))) {
        networks_ = scan();

        // Deduplicate WiFi SSIDs — keep strongest signal
        std::set<std::string> seen;
        std::vector<NetworkEntry> deduped;
        // Sort by RSSI descending first
        std::sort(networks_.begin(), networks_.end(),
                  [](const NetworkEntry& a, const NetworkEntry& b) {
                      return a.rssi > b.rssi;
                  });
        for (const auto& n : networks_) {
            if (n.type == "wifi") {
                if (seen.insert(n.name).second) {
                    deduped.push_back(n);
                }
            } else {
                deduped.push_back(n);
            }
        }
        networks_ = std::move(deduped);
        scanned_ = true;
    }

    ImGui::Spacing();

    // Network list
    if (networks_.empty() && scanned_) {
        ImGui::SetCursorPosX(60.0f);
        ImGui::TextDisabled("No networks found. Check hardware or skip.");
    }

    for (int i = 0; i < static_cast<int>(networks_.size()); ++i) {
        const auto& net = networks_[static_cast<size_t>(i)];
        ImGui::SetCursorPosX(60.0f);

        char label[256];
        if (net.type == "wifi") {
            int bars = rssi_to_bars(net.rssi);
            snprintf(label, sizeof(label), "[WiFi %d/4] %s%s",
                     bars, net.name.c_str(),
                     net.connected ? " (Connected)" : "");
        } else {
            snprintf(label, sizeof(label), "[Ethernet] %s%s",
                     net.name.c_str(),
                     net.connected ? " (Connected)" : "");
        }

        if (ImGui::Selectable(label, i == selected_)) {
            selected_ = i;
        }
    }

    // WiFi passphrase entry
    if (selected_ >= 0 &&
        selected_ < static_cast<int>(networks_.size()) &&
        networks_[static_cast<size_t>(selected_)].type == "wifi" &&
        !networks_[static_cast<size_t>(selected_)].connected) {
        ImGui::Spacing();
        ImGui::SetCursorPosX(60.0f);
        ImGui::Text("WPA Passphrase");
        ImGui::SetCursorPosX(60.0f);
        ImGui::SetNextItemWidth(300.0f);
        ImGui::InputText("##passphrase", passphrase_buf_,
                         sizeof(passphrase_buf_),
                         ImGuiInputTextFlags_Password);
        ImGui::SameLine();
        if (ImGui::Button("Connect")) {
            SL_INFO("Would connect to WiFi: {}",
                    networks_[static_cast<size_t>(selected_)].name);
            status_message_ = "Connecting...";
            // TODO: Activate connection via NetworkManager D-Bus
        }
    }

    // Status
    if (!status_message_.empty()) {
        ImGui::Spacing();
        ImGui::SetCursorPosX(60.0f);
        ImGui::Text("%s", status_message_.c_str());
    }

    ImGui::Spacing();
    ImGui::Spacing();

    // Buttons
    ImGui::SetCursorPosX(60.0f);
    bool advance = false;
    if (ImGui::Button("Next", ImVec2(120, 40))) {
        advance = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Skip", ImVec2(80, 40))) {
        advance = true;
    }

    ImGui::End();
    return advance;
}

} // namespace straylight::oobe
