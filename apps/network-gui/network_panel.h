// apps/network-gui/network_panel.h
// StrayLight Network Manager panel
#pragma once

#include <imgui.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <array>
#include <fstream>
#include <sstream>
// network-gui-os-includes

namespace straylight::network {

struct WiFiNetwork {
    char ssid[64];
    int  signal;      // 0-100
    bool secured;
    bool connected;
    char band[16];    // "2.4 GHz" or "5 GHz"
};

struct VPNConnection {
    char name[64];
    char protocol[32];
    char server[128];
    bool connected;
};

struct FirewallRule {
    int  id;
    char action[16];  // ALLOW/DENY
    char protocol[16];
    char source[64];
    char dest[64];
    int  port;
    bool enabled;
};

struct NetworkState {
    std::vector<WiFiNetwork> networks;
    std::vector<VPNConnection> vpns;
    std::vector<FirewallRule> fw_rules;

    int  selected_network = 0;
    bool show_password_dialog = false;
    char wifi_password[128] = {};

    // Connected info (populated from real OS reads in refresh()).
    char connected_ip[32] = "";
    char connected_dns[32] = "";
    char connected_gateway[32] = "";
    char connected_speed[32] = "";
    char connected_ssid[64] = "";

    // New firewall rule
    char new_fw_source[64] = {};
    char new_fw_dest[64] = {};
    int  new_fw_port = 0;
    int  new_fw_action = 0;
    int  new_fw_protocol = 0;
    bool show_add_fw = false;

    static constexpr const char* fw_actions[] = { "ALLOW", "DENY" };
    static constexpr const char* fw_protocols[] = { "TCP", "UDP", "ICMP", "ANY" };

    // OS-read link status (no daemon). No fabricated data.
    bool         ok_ = false;
    std::string  err_;
    double       last_refresh_ = -1.0e9;

    // ---- OS read helpers ------------------------------------------------

    // Run a command and capture trimmed stdout (first line by default).
    static std::string run_cmd(const char* cmd) {
        std::string out;
        FILE* p = popen(cmd, "r");
        if (!p) return out;
        char buf[512];
        while (fgets(buf, sizeof(buf), p)) out += buf;
        pclose(p);
        return out;
    }

    static std::string trim(const std::string& s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return "";
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }

    void refresh() {
        ok_ = false;
        err_.clear();

        // Default route line:
        //   default via 192.0.2.1 dev eth0 proto dhcp src 192.0.2.102 ...
        std::string route = run_cmd("ip route show default 2>/dev/null");
        std::string iface, gateway, srcip;
        {
            std::istringstream rs(route);
            std::string line;
            while (std::getline(rs, line)) {
                if (line.rfind("default", 0) != 0) continue;
                std::istringstream ls(line);
                std::string tok;
                while (ls >> tok) {
                    if (tok == "via")  { if (ls >> tok) gateway = tok; }
                    else if (tok == "dev") { if (ls >> tok) iface = tok; }
                    else if (tok == "src") { if (ls >> tok) srcip = tok; }
                }
                if (!iface.empty()) break;  // first default route wins
            }
        }

        if (iface.empty()) {
            err_ = "no default route (ip route show default returned nothing)";
            return;
        }

        // Source IP fallback: ip -o -4 addr show <iface>
        if (srcip.empty()) {
            std::string cmd = "ip -o -4 addr show " + iface + " 2>/dev/null";
            std::string addr = run_cmd(cmd.c_str());
            std::istringstream as(addr);
            std::string tok;
            while (as >> tok) {
                if (tok == "inet") { if (as >> tok) { size_t s = tok.find('/'); srcip = tok.substr(0, s); } break; }
            }
        }

        // DNS: first nameserver in /etc/resolv.conf
        std::string dns;
        {
            std::ifstream rc("/etc/resolv.conf");
            std::string line;
            while (std::getline(rc, line)) {
                std::istringstream ls(line);
                std::string kw;
                if (ls >> kw && kw == "nameserver") { ls >> dns; break; }
            }
        }

        // Link speed: /sys/class/net/<iface>/speed (Mbps)
        std::string speed;
        {
            std::ifstream sp("/sys/class/net/" + iface + "/speed");
            int mbps = 0;
            if (sp >> mbps && mbps > 0) speed = std::to_string(mbps) + " Mbps";
        }

        // Operational state (link-up indicator; no WiFi signal source).
        bool link_up = false;
        {
            std::ifstream os("/sys/class/net/" + iface + "/operstate");
            std::string st; if (os >> st) link_up = (st == "up");
        }
        if (speed.empty()) speed = link_up ? "link up" : "link down";

        // Commit connection-info card fields.
        snprintf(connected_ip, sizeof(connected_ip), "%s", srcip.c_str());
        snprintf(connected_dns, sizeof(connected_dns), "%s", dns.c_str());
        snprintf(connected_gateway, sizeof(connected_gateway), "%s", gateway.c_str());
        snprintf(connected_speed, sizeof(connected_speed), "%s", speed.c_str());
        snprintf(connected_ssid, sizeof(connected_ssid), "%s", iface.c_str());

        // Interface list from /proc/net/dev (NICs on this wired box).
        // signal/band/secured are WiFi-only and have no source here, so they
        // are left as a link-up indicator (signal = 100 if up, else 0).
        networks.clear();
        {
            std::ifstream nd("/proc/net/dev");
            std::string line;
            std::getline(nd, line); // header
            std::getline(nd, line); // header
            while (std::getline(nd, line)) {
                size_t colon = line.find(':');
                if (colon == std::string::npos) continue;
                std::string nm = trim(line.substr(0, colon));
                if (nm.empty() || nm == "lo") continue;
                bool up = false;
                {
                    std::ifstream os("/sys/class/net/" + nm + "/operstate");
                    std::string s; if (os >> s) up = (s == "up");
                }
                WiFiNetwork n{};
                snprintf(n.ssid, sizeof(n.ssid), "%s", nm.c_str());
                n.signal = up ? 100 : 0;          // link-up indicator, not RSSI
                n.secured = false;                 // no WiFi security info on wired NIC
                n.connected = (nm == iface);
                snprintf(n.band, sizeof(n.band), "%s", up ? "link up" : "link down");
                networks.push_back(n);
            }
        }

        // VPN and firewall have no OS source wired here: leave empty (never fabricate).
        vpns.clear();
        fw_rules.clear();

        if (connected_ip[0] == '\0' && connected_gateway[0] == '\0') {
            err_ = "could not read interface " + iface + " (no IP/gateway)";
            ok_ = false;
            return;
        }
        ok_ = true;
    }

    void maybe_refresh() {
        double now = ImGui::GetTime();
        if (now - last_refresh_ >= 2.0) {
            last_refresh_ = now;
            refresh();
        }
    }

    void init() {
        refresh();
    }
};

inline void render_network_panel(NetworkState& st) {
    st.maybe_refresh();
    // per-frame mock removed: data is loaded from real OS reads in refresh()
    if (!st.ok_) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.3f, 1.0f));
        ImGui::TextWrapped("network info unavailable: %s", st.err_.c_str());
        ImGui::PopStyleColor();
        if (ImGui::SmallButton("Retry")) st.refresh();
        ImGui::Separator();
    }

    const ImVec4 accent(0.0f, 1.0f, 0.67f, 1.0f);

    ImGui::PushStyleColor(ImGuiCol_Text, accent);
    ImGui::Text("NETWORK MANAGER");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();

    float left_w = ImGui::GetContentRegionAvail().x * 0.4f;

    // Left: WiFi list
    ImGui::BeginChild("##wifi_list", ImVec2(left_w, ImGui::GetContentRegionAvail().y * 0.55f), true);
    ImGui::TextColored(accent, "WiFi Networks");
    ImGui::Separator();

    for (int i = 0; i < (int)st.networks.size(); ++i) {
        auto& n = st.networks[i];
        ImGui::PushID(i);

        bool selected = (i == st.selected_network);
        char label[128];
        snprintf(label, 128, "%s %s%s", n.ssid, n.band, n.connected ? " [Connected]" : "");

        if (ImGui::Selectable(label, selected, 0, ImVec2(0, 36))) {
            st.selected_network = i;
        }

        // Signal strength bars
        ImVec2 pos = ImGui::GetItemRectMax();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        float bx = pos.x - 60;
        float by = pos.y - 28;
        int bars = n.signal > 75 ? 4 : n.signal > 50 ? 3 : n.signal > 25 ? 2 : 1;
        for (int b = 0; b < 4; ++b) {
            float h = 6 + b * 5;
            ImU32 col = (b < bars) ? IM_COL32(0, 255, 136, 255) : IM_COL32(60, 60, 80, 255);
            draw->AddRectFilled(ImVec2(bx + b * 10, by + 20 - h),
                                ImVec2(bx + b * 10 + 6, by + 20), col, 1.0f);
        }

        // Lock icon for secured
        if (n.secured) {
            draw->AddText(ImVec2(bx + 45, by + 4), IM_COL32(180, 180, 180, 255), "L");
        }

        ImGui::PopID();
    }

    ImGui::Spacing();
    if (ImGui::Button("Scan", ImVec2(-1, 28))) {
        // Trigger rescan
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Right: Connected info + VPN
    ImGui::BeginChild("##right_panel", ImVec2(0, ImGui::GetContentRegionAvail().y * 0.55f));

    // Connection info card
    ImGui::BeginChild("##conn_info", ImVec2(-1, 160), true);
    ImGui::TextColored(accent, "Connected: %s", st.connected_ssid);
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::Text("IP Address:"); ImGui::SameLine(140); ImGui::Text("%s", st.connected_ip);
    ImGui::Text("DNS Server:"); ImGui::SameLine(140); ImGui::Text("%s", st.connected_dns);
    ImGui::Text("Gateway:");    ImGui::SameLine(140); ImGui::Text("%s", st.connected_gateway);
    ImGui::Text("Link Speed:"); ImGui::SameLine(140); ImGui::Text("%s", st.connected_speed);
    ImGui::Spacing();
    if (ImGui::Button("Disconnect", ImVec2(120, 28))) {
        // disconnect
    }
    ImGui::SameLine();
    if (ImGui::Button("Forget", ImVec2(120, 28))) {
        // forget
    }
    ImGui::EndChild();

    // VPN
    ImGui::BeginChild("##vpn", ImVec2(-1, 0), true);
    ImGui::TextColored(accent, "VPN Connections");
    ImGui::Separator();
    for (int i = 0; i < (int)st.vpns.size(); ++i) {
        auto& v = st.vpns[i];
        ImGui::PushID(1000 + i);
        ImGui::Text("%s", v.name);
        ImGui::SameLine(200);
        ImGui::TextDisabled("(%s)", v.protocol);
        ImGui::SameLine(350);
        bool conn = v.connected;
        if (ImGui::Checkbox("##vpn_toggle", &conn)) {
            v.connected = conn;
        }
        ImGui::SameLine();
        ImGui::Text(v.connected ? "Connected" : "Disconnected");
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::EndChild();

    // Bottom: Firewall rules
    ImGui::BeginChild("##firewall", ImVec2(-1, 0), true);
    ImGui::TextColored(accent, "Firewall Rules");
    ImGui::SameLine(ImGui::GetWindowWidth() - 120);
    if (ImGui::SmallButton("Add Rule")) {
        st.show_add_fw = true;
        memset(st.new_fw_source, 0, sizeof(st.new_fw_source));
        memset(st.new_fw_dest, 0, sizeof(st.new_fw_dest));
        st.new_fw_port = 0;
    }
    ImGui::Separator();

    if (ImGui::BeginTable("##fw_table", 7,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 30);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Protocol", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Source");
        ImGui::TableSetupColumn("Destination");
        ImGui::TableSetupColumn("Port", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("Enabled", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableHeadersRow();

        for (int i = 0; i < (int)st.fw_rules.size(); ++i) {
            auto& r = st.fw_rules[i];
            ImGui::TableNextRow();
            ImGui::PushID(2000 + i);

            ImGui::TableSetColumnIndex(0); ImGui::Text("%d", r.id);
            ImGui::TableSetColumnIndex(1);
            ImVec4 ac = (strcmp(r.action, "ALLOW") == 0) ? ImVec4(0, 1, 0.67f, 1) : ImVec4(1, 0.3f, 0.3f, 1);
            ImGui::TextColored(ac, "%s", r.action);
            ImGui::TableSetColumnIndex(2); ImGui::Text("%s", r.protocol);
            ImGui::TableSetColumnIndex(3); ImGui::Text("%s", r.source);
            ImGui::TableSetColumnIndex(4); ImGui::Text("%s", r.dest);
            ImGui::TableSetColumnIndex(5);
            if (r.port > 0) ImGui::Text("%d", r.port); else ImGui::TextDisabled("*");
            ImGui::TableSetColumnIndex(6);
            ImGui::Checkbox("##en", &r.enabled);

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    // Add Firewall Rule Dialog
    if (st.show_add_fw) {
        ImGui::OpenPopup("Add Firewall Rule");
        st.show_add_fw = false;
    }
    if (ImGui::BeginPopupModal("Add Firewall Rule", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Combo("Action", &st.new_fw_action, NetworkState::fw_actions, 2);
        ImGui::Combo("Protocol", &st.new_fw_protocol, NetworkState::fw_protocols, 4);
        ImGui::InputText("Source", st.new_fw_source, sizeof(st.new_fw_source));
        ImGui::InputText("Destination", st.new_fw_dest, sizeof(st.new_fw_dest));
        ImGui::InputInt("Port", &st.new_fw_port);
        ImGui::Spacing();
        if (ImGui::Button("Add", ImVec2(120, 30))) {
            FirewallRule nr;
            nr.id = (int)st.fw_rules.size() + 1;
            snprintf(nr.action, 16, "%s", NetworkState::fw_actions[st.new_fw_action]);
            snprintf(nr.protocol, 16, "%s", NetworkState::fw_protocols[st.new_fw_protocol]);
            snprintf(nr.source, 64, "%s", strlen(st.new_fw_source) > 0 ? st.new_fw_source : "any");
            snprintf(nr.dest, 64, "%s", strlen(st.new_fw_dest) > 0 ? st.new_fw_dest : "any");
            nr.port = st.new_fw_port;
            nr.enabled = true;
            st.fw_rules.push_back(nr);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 30))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

} // namespace straylight::network
