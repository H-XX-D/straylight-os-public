// apps/mesh-gui/mesh_panel.h
// StrayLight Mesh GUI — GPU Mesh Dashboard panel
#pragma once

#include <imgui.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cmath>

namespace straylight::mesh {

struct GpuCard {
    std::string name;
    float       temperature;
    float       utilization; // 0-100
    float       vram_used;   // GB
    float       vram_total;  // GB
    float       power_watts;
};

struct MeshNode {
    std::string hostname;
    std::string ip;
    std::string status; // "online", "offline", "degraded"
    std::vector<GpuCard> gpus;
    float interconnect_bw; // GB/s to other nodes
};

struct MeshJob {
    int         id;
    std::string name;
    std::string command;
    std::string status; // "running", "queued", "completed", "failed"
    float       vram_required; // GB
    std::string placement;
    std::string node;
    float       progress;
};

struct MeshState {
    std::vector<MeshNode> nodes;
    std::vector<MeshJob> jobs;
    int selected_node = -1;

    // Submit job dialog
    bool show_submit_dialog = false;
    char job_name[128] = {};
    char job_command[512] = {};
    float job_vram = 8.0f;
    int  job_placement_idx = 0;
    int  next_job_id = 100;

        // [wired-mesh-gui]
    // OS data source: nvidia-smi via popen(); no daemon for this panel.
    bool        ok_ = false;
    std::string err_;
    double      last_refresh_ = -1.0e9;

    // Trim leading spaces (nvidia-smi CSV pads fields with a space).
    static std::string ltrim(std::string s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' ||
                              s.back() == '\r' || s.back() == '\n')) s.pop_back();
        return s;
    }

    // Read first line of a shell command into out; true on success.
    static bool read_cmd_line(const char* cmd, std::string& out) {
        FILE* pipe = popen(cmd, "r");
        if (!pipe) return false;
        char buf[512] = {};
        bool got = (fgets(buf, sizeof(buf), pipe) != nullptr);
        pclose(pipe);
        if (got) out = ltrim(std::string(buf));
        return got;
    }

    void ensure_connected() {
        // OS source: verify nvidia-smi exists once; record error if not.
        std::string which;
        bool present = read_cmd_line("which nvidia-smi 2>/dev/null", which) && !which.empty();
        if (!present) {
            ok_ = false;
            err_ = "nvidia-smi not found on PATH";
        }
    }

    void refresh() {
        ensure_connected();
        if (!err_.empty() && !ok_) {
            // ensure_connected already set err_ for a missing nvidia-smi.
        }
        // Build one node for this host from real nvidia-smi output.
        MeshNode node;
        std::string host, ip;
        if (read_cmd_line("hostname 2>/dev/null", host)) node.hostname = host;
        if (read_cmd_line("hostname -I 2>/dev/null", ip)) {
            // first token of `hostname -I`
            size_t sp = ip.find(' ');
            node.ip = (sp == std::string::npos) ? ip : ip.substr(0, sp);
        }
        node.status = "online";
        node.interconnect_bw = 0.0f; // no real source

        const char* cmd =
            "nvidia-smi --query-gpu="
            "index,name,temperature.gpu,utilization.gpu,memory.used,memory.total,power.draw "
            "--format=csv,noheader,nounits 2>/dev/null";
        FILE* pipe = popen(cmd, "r");
        if (!pipe) {
            ok_ = false;
            if (err_.empty()) err_ = "failed to run nvidia-smi";
            last_refresh_ = ImGui::GetTime();
            return;
        }
        bool any = false;
        char line[512];
        while (fgets(line, sizeof(line), pipe)) {
            int idx = 0;
            char name[256] = {};
            float temp = 0, util = 0, mem_used = 0, mem_total = 0, power = 0;
            int n = sscanf(line, "%d, %255[^,], %f, %f, %f, %f, %f",
                           &idx, name, &temp, &util, &mem_used, &mem_total, &power);
            if (n < 7) continue;
            GpuCard g;
            g.name        = ltrim(std::string(name));
            g.temperature = temp;
            g.utilization = util;
            g.vram_used   = mem_used / 1024.0f;   // MiB -> GB
            g.vram_total  = mem_total / 1024.0f;  // MiB -> GB
            g.power_watts = power;
            node.gpus.push_back(std::move(g));
            any = true;
        }
        pclose(pipe);

        if (any) {
            ok_ = true;
            err_.clear();
            nodes.clear();
            nodes.push_back(std::move(node));
        } else {
            ok_ = false;
            if (err_.empty()) err_ = "nvidia-smi returned no GPU rows";
        }
        last_refresh_ = ImGui::GetTime();
    }

    void maybe_refresh() {
        double now = ImGui::GetTime();
        if (now - last_refresh_ >= 2.0) refresh();
    }

    void init() { refresh(); }
};

inline void render_mesh_panel(MeshState& st) {
    // [wired-mesh-gui]
    st.maybe_refresh();
    if (!st.ok_) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.3f, 1.0f));
        ImGui::TextWrapped("mesh GPU data unavailable: %s", st.err_.c_str());
        ImGui::PopStyleColor();
        if (ImGui::SmallButton("Retry")) st.refresh();
        ImGui::Separator();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.67f, 1.0f));
    ImGui::Text("STRAYLIGHT MESH");
    ImGui::PopStyleColor();
    ImGui::SameLine(ImGui::GetWindowWidth() - 80);
    if (ImGui::SmallButton("Close")) {}
    ImGui::Separator();
    ImGui::Spacing();

    // Network topology view (top)
    float topo_h = 280.0f;
    if (ImGui::BeginChild("##topology", ImVec2(0, topo_h), true)) {
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.67f, 1.0f), "GPU Mesh Topology");
        ImGui::Separator();

        ImDrawList* draw = ImGui::GetWindowDrawList();
        ImVec2 origin = ImGui::GetCursorScreenPos();
        float avail_w = ImGui::GetContentRegionAvail().x;
        float node_w = 220.0f;
        float node_h = 140.0f;
        float spacing = (avail_w - node_w * (float)st.nodes.size()) / (float)(st.nodes.size() + 1);

        // Draw interconnect lines between online nodes
        std::vector<ImVec2> node_centers;
        for (int i = 0; i < (int)st.nodes.size(); ++i) {
            float x = origin.x + spacing + (node_w + spacing) * (float)i;
            float y = origin.y + 20;
            node_centers.push_back(ImVec2(x + node_w * 0.5f, y + node_h * 0.5f));
        }
        for (int i = 0; i < (int)st.nodes.size(); ++i) {
            for (int j = i + 1; j < (int)st.nodes.size(); ++j) {
                if (st.nodes[i].status != "offline" && st.nodes[j].status != "offline") {
                    float bw = std::min(st.nodes[i].interconnect_bw, st.nodes[j].interconnect_bw);
                    ImU32 line_col = bw >= 200 ? IM_COL32(0, 200, 130, 100) : IM_COL32(100, 150, 200, 80);
                    draw->AddLine(node_centers[i], node_centers[j], line_col, 2.0f);
                    // BW label at midpoint
                    ImVec2 mid = ImVec2((node_centers[i].x + node_centers[j].x) * 0.5f,
                                        (node_centers[i].y + node_centers[j].y) * 0.5f - 10);
                    char bw_label[32];
                    snprintf(bw_label, sizeof(bw_label), "%.0f GB/s", bw);
                    draw->AddText(mid, IM_COL32(140, 140, 180, 200), bw_label);
                }
            }
        }

        // Draw node boxes
        for (int ni = 0; ni < (int)st.nodes.size(); ++ni) {
            auto& n = st.nodes[ni];
            float x = origin.x + spacing + (node_w + spacing) * (float)ni;
            float y = origin.y + 20;

            ImU32 border_col = n.status == "online"   ? IM_COL32(0, 200, 130, 200)
                             : n.status == "degraded" ? IM_COL32(200, 180, 40, 200)
                             : IM_COL32(120, 120, 120, 100);
            ImU32 bg_col = IM_COL32(20, 20, 35, 200);

            draw->AddRectFilled(ImVec2(x, y), ImVec2(x + node_w, y + node_h), bg_col, 4.0f);
            draw->AddRect(ImVec2(x, y), ImVec2(x + node_w, y + node_h), border_col, 4.0f, 0, ni == st.selected_node ? 3.0f : 1.0f);

            // Node name
            draw->AddText(ImVec2(x + 8, y + 4), IM_COL32(220, 220, 220, 255), n.hostname.c_str());
            char ip_status[64];
            snprintf(ip_status, sizeof(ip_status), "%s [%s]", n.ip.c_str(), n.status.c_str());
            draw->AddText(ImVec2(x + 8, y + 20), IM_COL32(140, 140, 140, 200), ip_status);

            // GPU cards inside
            float gpu_y = y + 40;
            for (int gi = 0; gi < (int)n.gpus.size(); ++gi) {
                auto& g = n.gpus[gi];
                float gy = gpu_y + gi * 48.0f;

                // GPU name + temp
                char gpu_label[128];
                snprintf(gpu_label, sizeof(gpu_label), "%s", g.name.c_str());
                draw->AddText(ImVec2(x + 12, gy), IM_COL32(180, 180, 200, 255), gpu_label);

                if (n.status != "offline") {
                    char temp_label[32];
                    snprintf(temp_label, sizeof(temp_label), "%.0fC", g.temperature);
                    ImU32 temp_col = g.temperature < 60 ? IM_COL32(0, 200, 130, 255)
                                   : g.temperature < 80 ? IM_COL32(200, 180, 40, 255)
                                   : IM_COL32(200, 60, 60, 255);
                    draw->AddText(ImVec2(x + node_w - 40, gy), temp_col, temp_label);

                    // Utilization bar
                    float bar_x = x + 12;
                    float bar_y = gy + 16;
                    float bar_w = node_w - 24;
                    float bar_h = 6;
                    draw->AddRectFilled(ImVec2(bar_x, bar_y), ImVec2(bar_x + bar_w, bar_y + bar_h), IM_COL32(30, 30, 50, 255));
                    float util_w = bar_w * (g.utilization / 100.0f);
                    ImU32 util_col = g.utilization < 50 ? IM_COL32(0, 180, 120, 255)
                                   : g.utilization < 80 ? IM_COL32(200, 180, 40, 255)
                                   : IM_COL32(200, 60, 60, 255);
                    draw->AddRectFilled(ImVec2(bar_x, bar_y), ImVec2(bar_x + util_w, bar_y + bar_h), util_col);

                    // VRAM bar
                    bar_y += bar_h + 2;
                    draw->AddRectFilled(ImVec2(bar_x, bar_y), ImVec2(bar_x + bar_w, bar_y + bar_h), IM_COL32(30, 30, 50, 255));
                    float vram_frac = g.vram_used / g.vram_total;
                    float vram_w = bar_w * vram_frac;
                    ImU32 vram_col = vram_frac < 0.5f ? IM_COL32(60, 120, 200, 255)
                                   : vram_frac < 0.8f ? IM_COL32(200, 180, 40, 255)
                                   : IM_COL32(200, 60, 60, 255);
                    draw->AddRectFilled(ImVec2(bar_x, bar_y), ImVec2(bar_x + vram_w, bar_y + bar_h), vram_col);

                    // VRAM text
                    char vram_text[32];
                    snprintf(vram_text, sizeof(vram_text), "%.0f/%.0fGB", g.vram_used, g.vram_total);
                    draw->AddText(ImVec2(bar_x + bar_w + 2 - 70, bar_y - 2), IM_COL32(140, 140, 160, 200), vram_text);
                }
            }

            // Clickable area
            ImGui::SetCursorScreenPos(ImVec2(x, y));
            char btn_id[32];
            snprintf(btn_id, sizeof(btn_id), "##node_%d", ni);
            if (ImGui::InvisibleButton(btn_id, ImVec2(node_w, node_h))) {
                st.selected_node = ni;
            }
        }

        ImGui::Dummy(ImVec2(0, node_h + 30));
    }
    ImGui::EndChild();

    ImGui::Spacing();

    // Bottom: Job list + Submit
    if (ImGui::BeginChild("##job_section", ImVec2(0, -1), false)) {
        // Toolbar
        if (ImGui::Button("Submit Job", ImVec2(120, 28))) {
            st.show_submit_dialog = true;
            memset(st.job_name, 0, sizeof(st.job_name));
            memset(st.job_command, 0, sizeof(st.job_command));
            st.job_vram = 8.0f;
            st.job_placement_idx = 0;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Total VRAM: %.0f GB free across mesh",
                           []( const std::vector<MeshNode>& nodes) {
                               float total = 0;
                               for (auto& n : nodes) {
                                   if (n.status == "offline") continue;
                                   for (auto& g : n.gpus) total += g.vram_total - g.vram_used;
                               }
                               return total;
                           }(st.nodes));
        ImGui::Spacing();

        // Job table
        if (ImGui::BeginTable("##jobs", 7,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 40);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 90);
            ImGui::TableSetupColumn("VRAM", ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableSetupColumn("Placement", ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("Node(s)", ImGuiTableColumnFlags_WidthFixed, 180);
            ImGui::TableSetupColumn("Progress", ImGuiTableColumnFlags_WidthFixed, 120);
            ImGui::TableHeadersRow();

            for (auto& j : st.jobs) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%d", j.id);
                ImGui::TableNextColumn();
                ImGui::Text("%s", j.name.c_str());
                ImGui::TableNextColumn();
                ImVec4 status_col = j.status == "running"   ? ImVec4(0.2f, 1.0f, 0.5f, 1.0f)
                                  : j.status == "queued"    ? ImVec4(0.5f, 0.7f, 1.0f, 1.0f)
                                  : j.status == "completed" ? ImVec4(0.5f, 0.5f, 0.5f, 1.0f)
                                  : ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
                ImGui::TextColored(status_col, "%s", j.status.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%.0f GB", j.vram_required);
                ImGui::TableNextColumn();
                ImGui::Text("%s", j.placement.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%s", j.node.empty() ? "pending" : j.node.c_str());
                ImGui::TableNextColumn();
                if (j.status == "running") {
                    // [wired-mesh-gui] per-frame fake progress removed; no real job source.
                    ImGui::ProgressBar(j.progress, ImVec2(-1, 16));
                } else if (j.status == "completed") {
                    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.5f, 1.0f), "100%%");
                } else if (j.status == "failed") {
                    ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%.0f%%", j.progress * 100.0f);
                } else {
                    ImGui::TextDisabled("--");
                }
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();

    // Submit Job dialog
    if (st.show_submit_dialog) {
        ImGui::OpenPopup("Submit Job");
        st.show_submit_dialog = false;
    }
    if (ImGui::BeginPopupModal("Submit Job", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Job Name:");
        ImGui::SetNextItemWidth(400);
        ImGui::InputText("##job_name", st.job_name, sizeof(st.job_name));

        ImGui::Text("Command:");
        ImGui::SetNextItemWidth(400);
        ImGui::InputTextMultiline("##job_cmd", st.job_command, sizeof(st.job_command), ImVec2(400, 60));

        ImGui::Text("VRAM Required (GB):");
        ImGui::SetNextItemWidth(400);
        ImGui::SliderFloat("##vram_req", &st.job_vram, 1.0f, 160.0f, "%.0f GB", ImGuiSliderFlags_Logarithmic);

        ImGui::Text("Placement Strategy:");
        const char* placements[] = {"Any - first available", "Pack - minimize nodes", "Spread - maximize distribution"};
        ImGui::SetNextItemWidth(400);
        ImGui::Combo("##placement", &st.job_placement_idx, placements, 3);

        ImGui::Spacing();
        if (ImGui::Button("Submit", ImVec2(120, 30))) {
            if (strlen(st.job_name) > 0 && strlen(st.job_command) > 0) {
                MeshJob j;
                j.id = st.next_job_id++;
                j.name = st.job_name;
                j.command = st.job_command;
                j.status = "queued";
                j.vram_required = st.job_vram;
                const char* strats[] = {"any", "pack", "spread"};
                j.placement = strats[st.job_placement_idx];
                j.progress = 0.0f;
                st.jobs.push_back(j);
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 30))) { ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

} // namespace straylight::mesh
