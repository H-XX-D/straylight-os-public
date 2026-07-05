// apps/widgets/hpc/cluster_map.cpp
#include "cluster_map.h"
#include "widget_registry.h"

#include <imgui.h>

REGISTER_WIDGET(straylight::widgets::ClusterMapWidget, "cluster_map", "Cluster Map", straylight::widgets::WidgetCategory::HPC);
#include <cstdio>
#include <algorithm>

namespace straylight::widgets {

ImVec4 ClusterMapWidget::state_color(NodeState s) {
    switch (s) {
        case NodeState::Online:   return ImVec4(0.2f, 0.9f, 0.2f, 1);
        case NodeState::Degraded: return ImVec4(1.0f, 0.8f, 0.0f, 1);
        case NodeState::Offline:  return ImVec4(0.8f, 0.2f, 0.2f, 1);
        case NodeState::Draining: return ImVec4(0.5f, 0.5f, 1.0f, 1);
        case NodeState::Unknown:  return ImVec4(0.5f, 0.5f, 0.5f, 1);
    }
    return ImVec4(0.5f, 0.5f, 0.5f, 1);
}

const char* ClusterMapWidget::state_str(NodeState s) {
    switch (s) {
        case NodeState::Online:   return "Online";
        case NodeState::Degraded: return "Degraded";
        case NodeState::Offline:  return "Offline";
        case NodeState::Draining: return "Draining";
        case NodeState::Unknown:  return "Unknown";
    }
    return "Unknown";
}

void ClusterMapWidget::try_connect() {
    auto res = ipc_.connect("/run/straylight/registry.sock");
    connected_ = res.has_value();
    if (!connected_) error_msg_ = res.error();
}

void ClusterMapWidget::fetch_nodes() {
    if (!connected_) return;

    auto res = ipc_.command("cluster.nodes");
    if (!res.has_value()) {
        error_msg_ = res.error();
        connected_ = false;
        return;
    }

    auto& j = res.value();
    if (!j.contains("nodes") || !j["nodes"].is_array()) return;

    nodes_.clear();
    for (auto& nj : j["nodes"]) {
        ClusterNode n;
        n.hostname = nj.value("hostname", "");
        n.ip = nj.value("ip", "");
        n.state = static_cast<NodeState>(nj.value("state", 4));
        n.cpu_pct = nj.value("cpu_pct", 0.0f);
        n.gpu_pct = nj.value("gpu_pct", 0.0f);
        n.mem_pct = nj.value("mem_pct", 0.0f);
        n.gpu_count = nj.value("gpu_count", 0);
        n.cpu_cores = nj.value("cpu_cores", 0);
        n.mem_total_gb = nj.value("mem_total_gb", 0.0f);
        n.running_jobs = nj.value("running_jobs", 0);
        nodes_.push_back(std::move(n));
    }
}

void ClusterMapWidget::update() {
    if (!should_update()) return;
    if (!connected_) try_connect();
    if (connected_) fetch_nodes();
}

void ClusterMapWidget::render(bool* p_open) {
    if (!ImGui::Begin("Cluster Map", p_open)) {
        ImGui::End();
        return;
    }

    if (!connected_) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Disconnected from straylight-registry");
        if (!error_msg_.empty()) ImGui::TextWrapped("Error: %s", error_msg_.c_str());
        if (ImGui::Button("Retry")) try_connect();
        ImGui::End();
        return;
    }

    // Summary
    int online = 0, degraded = 0, offline = 0;
    for (auto& n : nodes_) {
        if (n.state == NodeState::Online) online++;
        else if (n.state == NodeState::Degraded) degraded++;
        else if (n.state == NodeState::Offline) offline++;
    }
    ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1), "%d online", online);
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1, 0.8f, 0, 1), "%d degraded", degraded);
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.8f, 0.2f, 0.2f, 1), "%d offline", offline);
    ImGui::SameLine();
    ImGui::Text("| Total: %zu nodes", nodes_.size());

    ImGui::SliderInt("Columns", &grid_cols_, 2, 16);
    ImGui::Separator();

    // Node grid
    float cell_size = 60.0f;
    float spacing = 4.0f;
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();

    for (int i = 0; i < static_cast<int>(nodes_.size()); ++i) {
        auto& n = nodes_[i];
        int col = i % grid_cols_;
        int row = i / grid_cols_;
        ImVec2 tl(origin.x + col * (cell_size + spacing),
                  origin.y + row * (cell_size + spacing));
        ImVec2 br(tl.x + cell_size, tl.y + cell_size);

        ImVec4 c = state_color(n.state);
        ImU32 fill = ImGui::GetColorU32(ImVec4(c.x * 0.3f, c.y * 0.3f, c.z * 0.3f, 0.8f));
        ImU32 border = ImGui::GetColorU32(c);

        draw_list->AddRectFilled(tl, br, fill, 4.0f);
        draw_list->AddRect(tl, br, border, 4.0f, 0, (selected_node_ == i) ? 3.0f : 1.0f);

        // CPU bar inside cell
        float bar_h = 4.0f;
        ImVec2 cpu_tl(tl.x + 2, br.y - bar_h - 2);
        ImVec2 cpu_br(tl.x + 2 + (cell_size - 4) * (n.cpu_pct / 100.0f), br.y - 2);
        draw_list->AddRectFilled(cpu_tl, ImVec2(tl.x + cell_size - 2, br.y - 2),
                                 ImGui::GetColorU32(ImVec4(0.2f, 0.2f, 0.2f, 0.5f)));
        draw_list->AddRectFilled(cpu_tl, cpu_br,
                                 ImGui::GetColorU32(ImVec4(0.3f, 0.7f, 1, 0.8f)));

        // Node index label
        char lbl[16];
        std::snprintf(lbl, sizeof(lbl), "%d", i);
        draw_list->AddText(ImVec2(tl.x + 4, tl.y + 2), ImGui::GetColorU32(ImVec4(1, 1, 1, 0.9f)), lbl);

        // Invisible button for hover/click
        ImGui::SetCursorScreenPos(tl);
        char btn_id[32];
        std::snprintf(btn_id, sizeof(btn_id), "##node_%d", i);
        if (ImGui::InvisibleButton(btn_id, ImVec2(cell_size, cell_size))) {
            selected_node_ = i;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text("%s (%s)", n.hostname.c_str(), n.ip.c_str());
            ImGui::Text("State: %s", state_str(n.state));
            ImGui::Text("CPU: %.0f%% | GPU: %.0f%% | Mem: %.0f%%",
                        n.cpu_pct, n.gpu_pct, n.mem_pct);
            ImGui::EndTooltip();
        }
    }

    // Advance cursor past grid
    int total_rows = (static_cast<int>(nodes_.size()) + grid_cols_ - 1) / grid_cols_;
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + total_rows * (cell_size + spacing) + spacing));
    ImGui::Dummy(ImVec2(0, 0));

    // Detail panel
    if (selected_node_ >= 0 && selected_node_ < static_cast<int>(nodes_.size())) {
        ImGui::Separator();
        auto& n = nodes_[selected_node_];
        ImGui::Text("Node: %s", n.hostname.c_str());
        ImGui::Text("IP: %s", n.ip.c_str());
        ImGui::TextColored(state_color(n.state), "State: %s", state_str(n.state));
        ImGui::Text("CPUs: %d cores | GPUs: %d", n.cpu_cores, n.gpu_count);
        ImGui::Text("Memory: %.1f GiB total", n.mem_total_gb);
        ImGui::Text("Running Jobs: %d", n.running_jobs);

        ImGui::Text("CPU"); ImGui::SameLine(80);
        char ov[32]; std::snprintf(ov, sizeof(ov), "%.0f%%", n.cpu_pct);
        ImGui::ProgressBar(n.cpu_pct / 100.0f, ImVec2(-1, 0), ov);

        ImGui::Text("GPU"); ImGui::SameLine(80);
        std::snprintf(ov, sizeof(ov), "%.0f%%", n.gpu_pct);
        ImGui::ProgressBar(n.gpu_pct / 100.0f, ImVec2(-1, 0), ov);

        ImGui::Text("Memory"); ImGui::SameLine(80);
        std::snprintf(ov, sizeof(ov), "%.0f%%", n.mem_pct);
        ImGui::ProgressBar(n.mem_pct / 100.0f, ImVec2(-1, 0), ov);
    }

    ImGui::End();
}

} // namespace straylight::widgets
