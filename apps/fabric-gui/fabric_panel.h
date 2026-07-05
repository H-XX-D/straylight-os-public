// apps/fabric-gui/fabric_panel.h
// StrayLight Device Topology (Fabric) panel
#pragma once

#include <imgui.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <fstream>
#include <sstream>
#include <map>
#include <algorithm>
#include <filesystem>

namespace straylight::fabric {

struct DeviceNode {
    char name[64];
    char type[32];     // "CPU", "GPU", "NVMe", "USB", "Network", "Memory", "PCIe Switch"
    float x, y;        // position in graph
    int  parent;       // -1 for root
    float bandwidth;   // GB/s
    float latency;     // ns
    int   affinity;    // NUMA node
};

struct PathResult {
    std::vector<int> nodes;
    float total_bandwidth;
    float total_latency;
    bool  has_bottleneck;
    int   bottleneck_node;
};

struct FabricState {
    std::vector<DeviceNode> devices;
    int selected_device = -1;
    int path_source = -1;
    int path_dest = -1;
    PathResult path_result;
    bool path_computed = false;

    // OS data source (PCIe topology from sysfs + lspci). No fabricated data.
    bool        ok_ = false;
    std::string err_;
    double      last_refresh_ = -1.0e9;

    // ---- helpers ---------------------------------------------------------
    static std::string read_first_line(const std::string& p) {
        std::ifstream f(p);
        if (!f.is_open()) return std::string();
        std::string s;
        std::getline(f, s);
        return s;
    }

    // PCIe class byte -> device type label used by the renderer.
    static const char* class_to_type(const std::string& cls) {
        // cls like "0x010802"; base class = first two hex digits after 0x.
        if (cls.size() < 6) return "PCIe";
        std::string base = cls.substr(2, 2);   // class
        std::string sub  = cls.substr(4, 2);   // subclass
        if (base == "01" && sub == "08") return "NVMe";
        if (base == "01") return "Storage";
        if (base == "03") return "GPU";
        if (base == "02") return "Network";
        if (base == "0c" && sub == "03") return "USB";
        if (base == "06") return "PCIe Switch";
        return "PCIe";
    }

    // PCIe link: GT/s x lanes -> approximate GB/s (matches panel's GB/s field).
    static float link_bandwidth(const std::string& speed, const std::string& width) {
        float gts = 0.0f;
        // speed like "8.0 GT/s PCIe"
        std::sscanf(speed.c_str(), "%f", &gts);
        int lanes = 0;
        try { lanes = std::stoi(width); } catch (...) { lanes = 0; }
        if (gts <= 0.0f || lanes <= 0) return 0.0f;
        // Per-lane GB/s: 2.5->0.5, 5.0->1.0, 8.0->1.97, 16.0->3.94, 32.0->7.88.
        float per_lane;
        if      (gts < 3.0f)  per_lane = 0.5f;    // PCIe1 2.5 GT/s
        else if (gts < 6.0f)  per_lane = 1.0f;    // PCIe2 5.0 GT/s
        else if (gts < 9.0f)  per_lane = 1.97f;   // PCIe3 8.0 GT/s
        else if (gts < 17.0f) per_lane = 3.94f;   // PCIe4 16.0 GT/s
        else                  per_lane = 7.88f;   // PCIe5 32.0 GT/s
        return per_lane * (float)lanes;
    }

    static std::string run_capture(const char* cmd) {
        std::string out;
        FILE* p = ::popen(cmd, "r");
        if (!p) return out;
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
        ::pclose(p);
        return out;
    }

    // Parse `lspci -mm` into a map: short BDF "16:00.0" -> device name string.
    static std::map<std::string, std::string> lspci_names() {
        std::map<std::string, std::string> m;
        std::string out = run_capture("lspci -mm 2>/dev/null");
        std::istringstream ss(out);
        std::string line;
        while (std::getline(ss, line)) {
            if (line.empty()) continue;
            // "16:00.0 \"Class\" \"Vendor\" \"Device\" ..."
            size_t sp = line.find(' ');
            if (sp == std::string::npos) continue;
            std::string bdf = line.substr(0, sp);
            // Collect quoted fields.
            std::vector<std::string> q;
            size_t i = sp;
            while (true) {
                size_t a = line.find('"', i);
                if (a == std::string::npos) break;
                size_t b = line.find('"', a + 1);
                if (b == std::string::npos) break;
                q.push_back(line.substr(a + 1, b - a - 1));
                i = b + 1;
            }
            // Fields: [0]=class, [1]=vendor, [2]=device.
            if (q.size() >= 3) {
                std::string name = q[1] + " " + q[2];
                m[bdf] = name;
            } else if (q.size() >= 1) {
                m[bdf] = q.back();
            }
        }
        return m;
    }

    // Short BDF: strip leading "0000:" domain if present.
    static std::string short_bdf(const std::string& full) {
        if (full.size() > 5 && full[4] == ':') return full.substr(5);
        return full;
    }

    void open_source() {
        // Nothing persistent to open for an OS source; presence of sysfs is
        // checked per-refresh.
    }

    void refresh() {
        ok_ = false;
        err_.clear();
        devices.clear();
        selected_device = -1;
        path_computed = false;
        path_result.nodes.clear();

        namespace fs = std::filesystem;
        const std::string pci_root = "/sys/bus/pci/devices";
        std::error_code ec;
        if (!fs::exists(pci_root, ec)) {
            err_ = "PCIe sysfs not available: " + pci_root;
            return;
        }

        // --- root complex anchors the real PCIe tree ---------------------
        DeviceNode root{};
        std::snprintf(root.name, sizeof(root.name), "Root Complex");
        std::snprintf(root.type, sizeof(root.type), "PCIe Switch");
        root.x = 400; root.y = 30; root.parent = -1;
        root.bandwidth = 0.0f; root.latency = 0.0f; root.affinity = 0;
        devices.push_back(root);
        const int root_idx = 0;

        // --- NUMA nodes (real, from /sys/devices/system/node) ------------
        std::map<int, int> numa_index;   // numa id -> device index
        {
            const std::string node_root = "/sys/devices/system/node";
            if (fs::exists(node_root, ec)) {
                std::vector<int> nodes;
                for (auto& e : fs::directory_iterator(node_root, ec)) {
                    std::string nm = e.path().filename().string();
                    if (nm.rfind("node", 0) == 0 && nm.size() > 4) {
                        int id = -1;
                        try { id = std::stoi(nm.substr(4)); } catch (...) { id = -1; }
                        if (id >= 0) nodes.push_back(id);
                    }
                }
                std::sort(nodes.begin(), nodes.end());
                float nx = 150.0f;
                for (int id : nodes) {
                    DeviceNode nd{};
                    std::snprintf(nd.name, sizeof(nd.name), "NUMA Node %d", id);
                    std::snprintf(nd.type, sizeof(nd.type), "CPU");
                    nd.x = nx; nd.y = 130; nd.parent = root_idx;
                    nd.bandwidth = 0.0f; nd.latency = 0.0f; nd.affinity = id;
                    numa_index[id] = (int)devices.size();
                    devices.push_back(nd);
                    nx += 500.0f;
                }
            }
        }
        auto parent_for_numa = [&](int numa)->int {
            auto it = numa_index.find(numa);
            if (it != numa_index.end()) return it->second;
            return root_idx;
        };

        // --- Memory: one node per NUMA node, sized from node meminfo -----
        for (auto& kv : numa_index) {
            int id = kv.first;
            std::string mip = "/sys/devices/system/node/node" + std::to_string(id) + "/meminfo";
            long kb = 0;
            std::ifstream mf(mip);
            std::string ml;
            while (std::getline(mf, ml)) {
                if (ml.find("MemTotal:") != std::string::npos) {
                    std::sscanf(ml.c_str(), "Node %*d MemTotal: %ld kB", &kb);
                    break;
                }
            }
            DeviceNode md{};
            if (kb > 0) {
                double gb = (double)kb / (1024.0 * 1024.0);
                std::snprintf(md.name, sizeof(md.name), "DRAM N%d (%.0fGB)", id, gb);
            } else {
                std::snprintf(md.name, sizeof(md.name), "DRAM N%d", id);
            }
            std::snprintf(md.type, sizeof(md.type), "Memory");
            md.x = 150.0f + 500.0f * (float)id; md.y = 230;
            md.parent = parent_for_numa(id);
            md.bandwidth = 0.0f; md.latency = 0.0f; md.affinity = id;
            devices.push_back(md);
        }

        // --- enumerate real PCIe endpoints -------------------------------
        std::map<std::string, std::string> names = lspci_names();
        struct Pci { std::string bdf; std::string type; float bw; int numa; };
        std::vector<Pci> pcis;

        for (auto& e : fs::directory_iterator(pci_root, ec)) {
            std::string bdf = e.path().filename().string();   // 0000:16:00.0
            std::string cls = read_first_line(e.path().string() + "/class");
            std::string type = class_to_type(cls);
            // Skip bridges/switches as leaf nodes (fabric, not endpoints).
            if (std::string("PCIe Switch") == type) continue;

            std::string speed = read_first_line(e.path().string() + "/current_link_speed");
            std::string width = read_first_line(e.path().string() + "/current_link_width");
            float bw = link_bandwidth(speed, width);

            // Surface meaningful endpoints only: a recognized leaf class, or a
            // function reporting a real PCIe link. This drops the hundreds of
            // CPU-internal config-space functions (CHA/CBDMA/IMC/etc.) that are
            // not user-facing fabric endpoints. Nothing fabricated: every kept
            // node is a real device read from sysfs.
            bool is_leaf =
                std::string("NVMe")    == type ||
                std::string("GPU")     == type ||
                std::string("Network") == type ||
                std::string("USB")     == type ||
                std::string("Storage") == type;
            if (!is_leaf && bw <= 0.0f) continue;

            std::string numa_s = read_first_line(e.path().string() + "/numa_node");
            int numa = 0;
            try { numa = std::stoi(numa_s); } catch (...) { numa = 0; }
            if (numa < 0) numa = 0;

            pcis.push_back({bdf, type, bw, numa});
        }

        // Place endpoints; parent onto their NUMA node (real affinity).
        float ex = 60.0f;
        for (auto& p : pcis) {
            DeviceNode d{};
            std::string sb = short_bdf(p.bdf);
            std::string nm = names.count(sb) ? names[sb] : sb;
            std::snprintf(d.name, sizeof(d.name), "%s", nm.c_str());
            std::snprintf(d.type, sizeof(d.type), "%s", p.type.c_str());
            d.x = ex; d.y = 360;
            d.parent = parent_for_numa(p.numa);
            d.bandwidth = p.bw;
            d.latency = 0.0f;            // no real per-device latency source
            d.affinity = p.numa;
            devices.push_back(d);
            ex += 120.0f;
            if (ex > 760.0f) ex = 60.0f;
        }

        ok_ = !devices.empty();
        if (!ok_) err_ = "no PCIe devices enumerated";
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

    void compute_path() {
        path_result.nodes.clear();
        path_computed = true;

        if (path_source < 0 || path_dest < 0) return;
        if (path_source >= (int)devices.size() || path_dest >= (int)devices.size()) return;

        // Walk up from source to root, then down to dest (real tree edges).
        std::vector<int> src_path, dst_path;
        int n = path_source;
        while (n >= 0) { src_path.push_back(n); n = devices[n].parent; }
        n = path_dest;
        while (n >= 0) { dst_path.push_back(n); n = devices[n].parent; }

        int lca = 0;
        for (int s : src_path) {
            for (int d : dst_path) {
                if (s == d) { lca = s; goto found; }
            }
        }
        found:

        for (int s : src_path) {
            path_result.nodes.push_back(s);
            if (s == lca) break;
        }
        std::vector<int> tail;
        for (int d : dst_path) {
            if (d == lca) break;
            tail.push_back(d);
        }
        for (int i = (int)tail.size() - 1; i >= 0; --i)
            path_result.nodes.push_back(tail[i]);

        path_result.total_bandwidth = 1.0e9f;
        path_result.total_latency = 0;
        path_result.has_bottleneck = false;
        path_result.bottleneck_node = -1;
        for (int nid : path_result.nodes) {
            float bw = devices[nid].bandwidth;
            if (bw > 0.0f && bw < path_result.total_bandwidth) {
                path_result.total_bandwidth = bw;
                if (path_result.total_bandwidth < 2.0f) {
                    path_result.has_bottleneck = true;
                    path_result.bottleneck_node = nid;
                }
            }
            path_result.total_latency += devices[nid].latency;
        }
        if (path_result.total_bandwidth >= 1.0e9f) path_result.total_bandwidth = 0.0f;
    }
};

inline void render_fabric_panel(FabricState& st) {
    st.maybe_refresh();
    if (!st.ok_) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.3f, 1.0f));
        ImGui::TextWrapped("device topology unavailable: %s", st.err_.c_str());
        ImGui::PopStyleColor();
        if (ImGui::SmallButton("Retry")) st.refresh();
        ImGui::Separator();
        return;
    }

    const ImVec4 accent(0.0f, 1.0f, 0.67f, 1.0f);

    ImGui::PushStyleColor(ImGuiCol_Text, accent);
    ImGui::Text("DEVICE TOPOLOGY");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();

    // Path query bar
    ImGui::Text("Path Query:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200);
    if (ImGui::BeginCombo("##src", st.path_source >= 0 ? st.devices[st.path_source].name : "Source")) {
        for (int i = 0; i < (int)st.devices.size(); ++i) {
            if (ImGui::Selectable(st.devices[i].name, st.path_source == i)) {
                st.path_source = i;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::Text("->");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200);
    if (ImGui::BeginCombo("##dst", st.path_dest >= 0 ? st.devices[st.path_dest].name : "Destination")) {
        for (int i = 0; i < (int)st.devices.size(); ++i) {
            if (ImGui::Selectable(st.devices[i].name, st.path_dest == i)) {
                st.path_dest = i;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Find Path", ImVec2(100, 0))) {
        st.compute_path();
    }

    // Path result
    if (st.path_computed && !st.path_result.nodes.empty()) {
        ImGui::SameLine(0, 20);
        ImGui::Text("BW: %.1f GB/s  Latency: %.0f ns", st.path_result.total_bandwidth, st.path_result.total_latency);
        if (st.path_result.has_bottleneck) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "BOTTLENECK: %s",
                               st.devices[st.path_result.bottleneck_node].name);
        }
    }
    ImGui::Spacing();

    float detail_w = 280;

    // Topology graph
    ImGui::BeginChild("##graph", ImVec2(ImGui::GetContentRegionAvail().x - detail_w - 8, 0), true);
    ImGui::TextDisabled("Device Tree (click to inspect)");

    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();

    // Draw edges
    for (int i = 0; i < (int)st.devices.size(); ++i) {
        auto& d = st.devices[i];
        if (d.parent >= 0) {
            auto& p = st.devices[d.parent];
            ImVec2 from(origin.x + p.x + 40, origin.y + p.y + 20);
            ImVec2 to(origin.x + d.x + 40, origin.y + d.y);

            // Check if this edge is on the path
            bool on_path = false;
            if (st.path_computed) {
                for (int j = 0; j < (int)st.path_result.nodes.size() - 1; ++j) {
                    int a = st.path_result.nodes[j], b = st.path_result.nodes[j+1];
                    if ((a == i && b == d.parent) || (a == d.parent && b == i)) {
                        on_path = true; break;
                    }
                }
            }

            ImU32 edge_col = on_path ? IM_COL32(0, 255, 136, 255) : IM_COL32(60, 60, 80, 200);
            float thickness = on_path ? 3.0f : 1.5f;
            draw->AddLine(from, to, edge_col, thickness);
        }
    }

    // Draw nodes
    ImGuiIO& io = ImGui::GetIO();
    for (int i = 0; i < (int)st.devices.size(); ++i) {
        auto& d = st.devices[i];
        ImVec2 tl(origin.x + d.x, origin.y + d.y);
        ImVec2 br(tl.x + 80, tl.y + 40);

        bool hovered = io.MousePos.x >= tl.x && io.MousePos.x <= br.x &&
                       io.MousePos.y >= tl.y && io.MousePos.y <= br.y;

        // Node color by type
        ImU32 node_col;
        if (strcmp(d.type, "CPU") == 0) node_col = IM_COL32(0, 100, 180, 200);
        else if (strcmp(d.type, "GPU") == 0) node_col = IM_COL32(0, 160, 80, 200);
        else if (strcmp(d.type, "Memory") == 0) node_col = IM_COL32(120, 80, 160, 200);
        else if (strcmp(d.type, "NVMe") == 0) node_col = IM_COL32(180, 100, 0, 200);
        else if (strcmp(d.type, "Network") == 0) node_col = IM_COL32(0, 120, 160, 200);
        else if (strcmp(d.type, "USB") == 0) node_col = IM_COL32(100, 100, 100, 200);
        else node_col = IM_COL32(60, 60, 100, 200);

        // Bottleneck highlight
        bool is_bottleneck = st.path_result.has_bottleneck && st.path_result.bottleneck_node == i;
        if (is_bottleneck) node_col = IM_COL32(255, 50, 50, 220);

        bool selected = (st.selected_device == i);
        ImU32 border = selected ? IM_COL32(0, 255, 136, 255) :
                       hovered ? IM_COL32(200, 200, 200, 255) :
                       IM_COL32(80, 80, 100, 255);

        draw->AddRectFilled(tl, br, node_col, 6.0f);
        draw->AddRect(tl, br, border, 6.0f, 0, selected ? 3.0f : 1.5f);

        // Truncated name
        char short_name[20];
        snprintf(short_name, 20, "%.19s", d.name);
        draw->AddText(ImVec2(tl.x + 4, tl.y + 4), IM_COL32(255, 255, 255, 255), short_name);
        draw->AddText(ImVec2(tl.x + 4, tl.y + 22), IM_COL32(180, 180, 180, 200), d.type);

        if (hovered && ImGui::IsMouseClicked(0)) {
            st.selected_device = i;
        }
    }

    ImGui::Dummy(ImVec2(0, 560));
    ImGui::EndChild();

    ImGui::SameLine();

    // Device detail panel
    ImGui::BeginChild("##detail", ImVec2(detail_w, 0), true);
    if (st.selected_device >= 0 && st.selected_device < (int)st.devices.size()) {
        auto& d = st.devices[st.selected_device];

        ImGui::TextColored(accent, "%s", d.name);
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("Type:");       ImGui::SameLine(100); ImGui::Text("%s", d.type);
        ImGui::Text("Bandwidth:");  ImGui::SameLine(100); ImGui::Text("%.1f GB/s", d.bandwidth);
        ImGui::Text("Latency:");    ImGui::SameLine(100); ImGui::Text("%.0f ns", d.latency);
        ImGui::Text("NUMA Node:");  ImGui::SameLine(100); ImGui::Text("%d", d.affinity);

        if (d.parent >= 0) {
            ImGui::Text("Parent:");
            ImGui::SameLine(100);
            ImGui::TextColored(accent, "%s", st.devices[d.parent].name);
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextColored(accent, "Performance");
        ImGui::Spacing();

        // Bandwidth bar
        float max_bw = 76.8f;
        float bw_frac = d.bandwidth / max_bw;
        char bw_str[32]; snprintf(bw_str, 32, "%.1f GB/s", d.bandwidth);
        ImGui::Text("Bandwidth:");
        ImGui::ProgressBar(bw_frac, ImVec2(-1, 16), bw_str);

        // Latency bar (inverse - lower is better)
        float max_lat = 5000.0f;
        float lat_frac = d.latency / max_lat;
        char lat_str[32]; snprintf(lat_str, 32, "%.0f ns", d.latency);
        ImGui::Text("Latency:");
        ImGui::ProgressBar(lat_frac, ImVec2(-1, 16), lat_str);

        ImGui::Spacing();
        ImGui::Separator();

        // Quick path buttons
        ImGui::TextColored(accent, "Quick Path");
        if (ImGui::Button("Set as Source", ImVec2(-1, 24))) {
            st.path_source = st.selected_device;
        }
        if (ImGui::Button("Set as Destination", ImVec2(-1, 24))) {
            st.path_dest = st.selected_device;
        }
    } else {
        ImGui::TextDisabled("Click a device in the graph");
    }
    ImGui::EndChild();
}

} // namespace straylight::fabric
