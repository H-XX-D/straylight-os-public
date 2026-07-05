// apps/probe-gui/scan_panel.h
// StrayLight Probe GUI — Network Scanner panel
#pragma once

#include <imgui.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <algorithm>
// [wire] os data-source headers
#include <array>
#include <fstream>
#include <sstream>
#include <cstdint>
#include <ctime>
#include <cstdlib>
#include <cctype>

namespace straylight::probe {

struct HostResult {
    std::string ip;
    std::string mac;
    std::string vendor;
    std::string hostname;
    std::string open_ports;
    bool        alive = true;
};

struct TracerouteHop {
    int         hop_num;
    std::string ip;
    std::string hostname;
    float       latency_ms;
    bool        timeout = false;
};

struct ScanState {
    // Scan tab
    char subnet_input[128] = "192.0.2.0/24";
    bool scanning = false;
    float scan_progress = 0.0f;
    std::vector<HostResult> hosts;

    // Port scan tab
    char port_host[128] = "192.0.2.1";
    char port_range[64] = "1-1024";
    int  scan_type_idx = 0;
    bool port_scanning = false;
    float port_progress = 0.0f;
    std::vector<std::pair<int, std::string>> port_results;

    // Traceroute tab
    char trace_host[128] = "8.8.8.8";
    bool tracing = false;
    std::vector<TracerouteHop> trace_hops;

    // Bandwidth tab
    char bw_host[128] = "speedtest.local";
    bool bw_testing = false;
    float bw_progress = 0.0f;
    float bw_download = 0.0f;
    float bw_upload = 0.0f;
    float bw_history[120] = {};
    int   bw_history_offset = 0;

    // Network health
    struct HealthIndicator {
        std::string name;
        int status; // 0=green, 1=yellow, 2=red
        std::string detail;
    };
    std::vector<HealthIndicator> health_indicators;

    int active_tab = 0;

    // [wire] Live OS data source (no fabricated data). source_kind=os.
    bool        ok_ = false;
    std::string err_;
    double      last_refresh_ = -1.0e9;

    // [wire] popen helper (proven pattern from apps/hub/network_panel.cpp).
    static std::string exec_cmd(const std::string& cmd) {
        std::array<char, 4096> buf{};
        std::string result;
        FILE* pipe = ::popen(cmd.c_str(), "r");
        if (!pipe) return std::string();
        while (::fgets(buf.data(), (int)buf.size(), pipe)) result += buf.data();
        ::pclose(pipe);
        return result;
    }

    static std::string trim(const std::string& s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return std::string();
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }

    // [wire] Best-effort vendor from MAC OUI prefix. No local IEEE OUI DB is
    // installed on this box, so this is a coarse prefix label only.
    static std::string vendor_from_mac(const std::string& mac) {
        if (mac.size() < 8) return std::string();
        std::string p = mac.substr(0, 8);
        for (auto& c : p) c = (char)std::toupper((unsigned char)c);
        return std::string("OUI ") + p;
    }

    static std::string getent_hostname(const std::string& ip) {
        // getent hosts <ip> -> "<ip>\t<name>"; empty if unresolved.
        std::string out = exec_cmd("getent hosts " + ip + " 2>/dev/null");
        std::istringstream ss(out);
        std::string addr, name;
        if (ss >> addr >> name) return name;
        return std::string();
    }

    // [wire] Read default gateway IP from `ip route`.
    static std::string default_gateway() {
        std::string out = exec_cmd("ip route 2>/dev/null");
        std::istringstream ss(out);
        std::string line;
        while (std::getline(ss, line)) {
            if (line.rfind("default", 0) == 0) {
                std::istringstream ls(line);
                std::string tok;
                while (ls >> tok) {
                    if (tok == "via") { std::string gw; if (ls >> gw) return gw; }
                }
            }
        }
        return std::string();
    }

    // [wire] Load the live neighbor/ARP table into hosts (IP, MAC, alive,
    // hostname, vendor). open_ports left empty: no nmap on this box.
    void load_hosts() {
        hosts.clear();
        std::string out = exec_cmd("ip neigh 2>/dev/null");
        std::istringstream ss(out);
        std::string line;
        std::vector<std::string> seen_ips;
        while (std::getline(ss, line)) {
            std::istringstream ls(line);
            std::string ip;
            if (!(ls >> ip)) continue;
            // Skip IPv6 link-local clutter; keep IPv4 entries.
            if (ip.find(':') != std::string::npos) continue;
            std::string tok, mac, state;
            while (ls >> tok) {
                if (tok == "lladdr") { ls >> mac; }
                else if (tok == "REACHABLE" || tok == "STALE" || tok == "DELAY" ||
                         tok == "PROBE" || tok == "FAILED" || tok == "INCOMPLETE" ||
                         tok == "NOARP" || tok == "PERMANENT") { state = tok; }
            }
            if (mac.empty()) continue;
            // De-dup identical IPs (multiple devices share the same neighbor).
            bool dup = false;
            for (auto& s : seen_ips) if (s == ip) { dup = true; break; }
            if (dup) continue;
            seen_ips.push_back(ip);

            HostResult h;
            h.ip       = ip;
            h.mac      = mac;
            h.vendor   = vendor_from_mac(mac);
            h.hostname = getent_hostname(ip);
            h.open_ports.clear(); // no port scanner installed (no nmap)
            h.alive    = (state == "REACHABLE");
            hosts.push_back(std::move(h));
        }
        std::sort(hosts.begin(), hosts.end(),
                  [](const HostResult& a, const HostResult& b) { return a.ip < b.ip; });
    }

    // [wire] Count IPv4 ARP entries in /proc/net/arp (data rows only).
    static int arp_entry_count() {
        std::ifstream f("/proc/net/arp");
        if (!f.is_open()) return 0;
        std::string line;
        int n = 0, ln = 0;
        while (std::getline(f, line)) {
            if (++ln == 1) continue; // header
            if (trim(line).empty()) continue;
            ++n;
        }
        return n;
    }

    // [wire] Build the Health Overview from real signals. Wireable parts only:
    // gateway reachability + latency/loss (ping), interface counters
    // (/proc/net/dev), ARP entry count (/proc/net/arp).
    void load_health() {
        health_indicators.clear();

        std::string gw = default_gateway();

        // Gateway reachability + latency + loss via a single ping.
        if (!gw.empty()) {
            std::string p = exec_cmd("ping -c1 -W1 " + gw + " 2>/dev/null");
            bool reach = p.find(" bytes from ") != std::string::npos ||
                         p.find("bytes from ") != std::string::npos;
            // Parse rtt time=NN ms
            float rtt = 0.0f; bool have_rtt = false;
            size_t tp = p.find("time=");
            if (tp != std::string::npos) {
                rtt = (float)std::atof(p.c_str() + tp + 5);
                have_rtt = true;
            }
            // Parse packet loss percentage.
            float loss = 0.0f; bool have_loss = false;
            size_t lp = p.find("% packet loss");
            if (lp != std::string::npos) {
                size_t start = p.rfind(' ', lp);
                if (start != std::string::npos) {
                    loss = (float)std::atof(p.c_str() + start + 1);
                    have_loss = true;
                }
            }
            {
                HealthIndicator ind;
                ind.name   = "Gateway Reachable";
                ind.status = reach ? 0 : 2;
                ind.detail = gw + (reach ? " responds to ping" : " no response");
                health_indicators.push_back(std::move(ind));
            }
            if (have_rtt) {
                HealthIndicator ind;
                ind.name   = "Latency";
                ind.status = rtt < 10.0f ? 0 : (rtt < 50.0f ? 1 : 2);
                char d[64]; snprintf(d, sizeof(d), "%.2f ms to gateway", rtt);
                ind.detail = d;
                health_indicators.push_back(std::move(ind));
            }
            if (have_loss) {
                HealthIndicator ind;
                ind.name   = "Packet Loss";
                ind.status = loss <= 0.0f ? 0 : (loss < 50.0f ? 1 : 2);
                char d[64]; snprintf(d, sizeof(d), "%.0f%% packet loss to gateway", loss);
                ind.detail = d;
                health_indicators.push_back(std::move(ind));
            }
        }

        // Interface counters from /proc/net/dev (first non-lo interface that
        // is up / has traffic). Proven parse pattern from system_monitor.
        {
            std::ifstream f("/proc/net/dev");
            if (f.is_open()) {
                std::string line;
                int ln = 0;
                while (std::getline(f, line)) {
                    if (++ln <= 2) continue; // two header lines
                    std::istringstream ls(line);
                    std::string name;
                    ls >> name;
                    if (!name.empty() && name.back() == ':') name.pop_back();
                    if (name.empty() || name == "lo") continue;
                    uint64_t rxb = 0, rxp = 0, d = 0;
                    ls >> rxb >> rxp >> d >> d >> d >> d >> d >> d;
                    uint64_t txb = 0, txp = 0;
                    ls >> txb >> txp;
                    if (rxb == 0 && txb == 0) continue;
                    HealthIndicator ind;
                    ind.name   = "Interface " + name;
                    ind.status = 0;
                    char det[160];
                    snprintf(det, sizeof(det),
                             "RX %llu B / %llu pkts, TX %llu B / %llu pkts",
                             (unsigned long long)rxb, (unsigned long long)rxp,
                             (unsigned long long)txb, (unsigned long long)txp);
                    ind.detail = det;
                    health_indicators.push_back(std::move(ind));
                    break; // primary interface only
                }
            }
        }

        // ARP table entry count from /proc/net/arp.
        {
            int n = arp_entry_count();
            HealthIndicator ind;
            ind.name   = "ARP Table";
            ind.status = 0;
            ind.detail = std::to_string(n) + " entries";
            health_indicators.push_back(std::move(ind));
        }
    }

    // [wire] Load all real OS data into the SAME fields render reads.
    void refresh() {
        ok_ = false;
        err_.clear();

        // Minimal availability probe: we need the neighbor table.
        std::ifstream arp("/proc/net/arp");
        if (!arp.is_open()) {
            err_ = "cannot read /proc/net/arp (neighbor table unavailable)";
            return;
        }
        arp.close();

        load_hosts();
        load_health();

        ok_ = true;

        // Fields with no real source on this box are left empty/zero:
        //   - port_results: no nmap installed (Port Scan not wireable)
        //   - trace_hops:   no traceroute installed (Traceroute not wireable)
        //   - bw_download / bw_upload / bw_history: no speedtest tooling
        //     (Bandwidth tab is not wireable; never fabricated).
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

inline void render_scan_panel(ScanState& st) {
    st.maybe_refresh();
    if (!st.ok_) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.3f, 1.0f));
        ImGui::TextWrapped("network data unavailable: %s", st.err_.c_str());
        ImGui::PopStyleColor();
        if (ImGui::SmallButton("Retry")) st.refresh();
        ImGui::Separator();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.67f, 1.0f));
    ImGui::Text("STRAYLIGHT PROBE");
    ImGui::PopStyleColor();
    ImGui::SameLine(ImGui::GetWindowWidth() - 80);
    if (ImGui::SmallButton("Close")) { /* handled by main loop */ }
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::BeginTabBar("##probe_tabs")) {
        // --- Network Scan Tab ---
        if (ImGui::BeginTabItem("Network Scan")) {
            ImGui::Spacing();
            ImGui::SetNextItemWidth(250);
            ImGui::InputTextWithHint("##subnet", "Subnet (e.g. 192.0.2.0/24)", st.subnet_input, sizeof(st.subnet_input));
            ImGui::SameLine();
            if (st.scanning) {
                ImGui::BeginDisabled();
                ImGui::Button("Scanning...", ImVec2(120, 0));
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::SetNextItemWidth(200);
                ImGui::ProgressBar(st.scan_progress, ImVec2(200, 0));
                st.scan_progress += ImGui::GetIO().DeltaTime * 0.15f;
                if (st.scan_progress >= 1.0f) {
                    st.scanning = false;
                    st.scan_progress = 1.0f;
                }
            } else {
                if (ImGui::Button("Scan", ImVec2(120, 0))) {
                    st.scanning = true;
                    st.scan_progress = 0.0f;
                }
                if (st.scan_progress > 0.0f) {
                    ImGui::SameLine();
                    ImGui::Text("%zu hosts found", st.hosts.size());
                }
            }
            ImGui::Spacing();

            if (ImGui::BeginTable("##scan_results", 5,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                    ImGuiTableFlags_ScrollY, ImVec2(0, -1))) {
                ImGui::TableSetupColumn("IP Address", ImGuiTableColumnFlags_WidthFixed, 140);
                ImGui::TableSetupColumn("MAC Address", ImGuiTableColumnFlags_WidthFixed, 160);
                ImGui::TableSetupColumn("Vendor", ImGuiTableColumnFlags_WidthFixed, 140);
                ImGui::TableSetupColumn("Hostname", ImGuiTableColumnFlags_WidthFixed, 160);
                ImGui::TableSetupColumn("Open Ports", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();

                for (auto& h : st.hosts) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.6f, 1.0f), "%s", h.ip.c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%s", h.mac.c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%s", h.vendor.c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%s", h.hostname.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.4f, 1.0f), "%s", h.open_ports.c_str());
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }

        // --- Port Scan Tab ---
        if (ImGui::BeginTabItem("Port Scan")) {
            ImGui::Spacing();
            ImGui::SetNextItemWidth(200);
            ImGui::InputTextWithHint("##port_host", "Host IP", st.port_host, sizeof(st.port_host));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120);
            ImGui::InputTextWithHint("##port_range", "Port range", st.port_range, sizeof(st.port_range));
            ImGui::SameLine();
            const char* scan_types[] = {"TCP Connect", "SYN Scan", "UDP Scan", "FIN Scan"};
            ImGui::SetNextItemWidth(140);
            ImGui::Combo("##scan_type", &st.scan_type_idx, scan_types, 4);
            ImGui::SameLine();
            if (st.port_scanning) {
                ImGui::BeginDisabled();
                ImGui::Button("Scanning...");
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::ProgressBar(st.port_progress, ImVec2(150, 0));
                st.port_progress += ImGui::GetIO().DeltaTime * 0.2f;
                if (st.port_progress >= 1.0f) { st.port_scanning = false; st.port_progress = 1.0f; }
            } else {
                if (ImGui::Button("Scan Ports")) {
                    st.port_scanning = true;
                    st.port_progress = 0.0f;
                }
            }
            ImGui::Spacing();

            if (ImGui::BeginTable("##port_results", 3,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, -1))) {
                ImGui::TableSetupColumn("Port", ImGuiTableColumnFlags_WidthFixed, 100);
                ImGui::TableSetupColumn("Service", ImGuiTableColumnFlags_WidthFixed, 200);
                ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();
                for (auto& [port, svc] : st.port_results) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", port);
                    ImGui::TableNextColumn();
                    ImGui::Text("%s", svc.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "OPEN");
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }

        // --- Traceroute Tab ---
        if (ImGui::BeginTabItem("Traceroute")) {
            ImGui::Spacing();
            ImGui::SetNextItemWidth(250);
            ImGui::InputTextWithHint("##trace_host", "Target host", st.trace_host, sizeof(st.trace_host));
            ImGui::SameLine();
            if (st.tracing) {
                ImGui::BeginDisabled();
                ImGui::Button("Tracing...");
                ImGui::EndDisabled();
            } else {
                if (ImGui::Button("Trace Route")) { st.tracing = false; /* already have data */ }
            }
            ImGui::Spacing();

            if (ImGui::BeginTable("##trace_results", 4,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, -1))) {
                ImGui::TableSetupColumn("Hop", ImGuiTableColumnFlags_WidthFixed, 60);
                ImGui::TableSetupColumn("IP Address", ImGuiTableColumnFlags_WidthFixed, 180);
                ImGui::TableSetupColumn("Hostname", ImGuiTableColumnFlags_WidthFixed, 200);
                ImGui::TableSetupColumn("Latency (ms)", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();
                for (auto& hop : st.trace_hops) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", hop.hop_num);
                    ImGui::TableNextColumn();
                    if (hop.timeout) {
                        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "*");
                    } else {
                        ImGui::Text("%s", hop.ip.c_str());
                    }
                    ImGui::TableNextColumn();
                    ImGui::Text("%s", hop.hostname.c_str());
                    ImGui::TableNextColumn();
                    if (hop.timeout) {
                        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "* * *");
                    } else {
                        // Color by latency
                        ImVec4 col = hop.latency_ms < 10 ? ImVec4(0.2f, 1.0f, 0.4f, 1.0f)
                                   : hop.latency_ms < 50 ? ImVec4(1.0f, 0.8f, 0.2f, 1.0f)
                                   : ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
                        ImGui::TextColored(col, "%.1f ms", hop.latency_ms);

                        // Latency bar
                        ImGui::SameLine();
                        float frac = std::min(hop.latency_ms / 100.0f, 1.0f);
                        ImGui::ProgressBar(frac, ImVec2(100, 14), "");
                    }
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }

        // --- Bandwidth Tab ---
        if (ImGui::BeginTabItem("Bandwidth")) {
            ImGui::Spacing();
            ImGui::SetNextItemWidth(250);
            ImGui::InputTextWithHint("##bw_host", "Test server", st.bw_host, sizeof(st.bw_host));
            ImGui::SameLine();
            if (st.bw_testing) {
                ImGui::BeginDisabled();
                ImGui::Button("Testing...");
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::ProgressBar(st.bw_progress, ImVec2(150, 0));
                st.bw_progress += ImGui::GetIO().DeltaTime * 0.1f;
                if (st.bw_progress >= 1.0f) { st.bw_testing = false; st.bw_progress = 1.0f; }
            } else {
                if (ImGui::Button("Run Test")) {
                    st.bw_testing = true;
                    st.bw_progress = 0.0f;
                }
            }
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Results
            ImGui::Columns(2, "##bw_cols", false);
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.67f, 1.0f), "Download");
            ImGui::Text("%.1f Mbps", st.bw_download);
            ImGui::ProgressBar(st.bw_download / 1000.0f, ImVec2(-1, 20));
            ImGui::NextColumn();
            ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), "Upload");
            ImGui::Text("%.1f Mbps", st.bw_upload);
            ImGui::ProgressBar(st.bw_upload / 1000.0f, ImVec2(-1, 20));
            ImGui::Columns(1);

            ImGui::Spacing();
            ImGui::Text("Throughput over time:");

            // Sparkline / plot
            ImGui::PlotLines("##bw_graph", st.bw_history, 120, st.bw_history_offset,
                             "Mbps", 0.0f, 1200.0f, ImVec2(-1, 200));
            // [wire] Per-frame mock removed: no speedtest tooling on this
            // box, so bw_history has no real source and is left at zero.

            ImGui::EndTabItem();
        }

        // --- Network Health Tab ---
        if (ImGui::BeginTabItem("Health Overview")) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.67f, 1.0f), "Network Health Status");
            ImGui::Separator();
            ImGui::Spacing();

            for (auto& ind : st.health_indicators) {
                ImVec4 color;
                const char* status_str;
                switch (ind.status) {
                    case 0: color = ImVec4(0.2f, 1.0f, 0.4f, 1.0f); status_str = "[OK]   "; break;
                    case 1: color = ImVec4(1.0f, 0.8f, 0.2f, 1.0f); status_str = "[WARN] "; break;
                    default: color = ImVec4(1.0f, 0.3f, 0.3f, 1.0f); status_str = "[FAIL] "; break;
                }
                ImGui::TextColored(color, "%s", status_str);
                ImGui::SameLine();
                ImGui::Text("%s", ind.name.c_str());
                ImGui::SameLine(350);
                ImGui::TextDisabled("%s", ind.detail.c_str());
                ImGui::Spacing();
            }

            // Summary bar
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            int ok = 0, warn = 0, fail = 0;
            for (auto& ind : st.health_indicators) {
                if (ind.status == 0) ok++;
                else if (ind.status == 1) warn++;
                else fail++;
            }
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "%d OK", ok);
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "%d Warning", warn);
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%d Critical", fail);

            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
}

} // namespace straylight::probe
