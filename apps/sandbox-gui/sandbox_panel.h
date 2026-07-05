// apps/sandbox-gui/sandbox_panel.h
// StrayLight Sandbox GUI — Container Manager panel
#pragma once

#include <imgui.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cmath>

namespace straylight::sandbox {

struct SandboxEntry {
    std::string name;
    std::string status; // "running", "stopped", "paused"
    float cpu_usage;    // 0-100
    float mem_usage;    // 0-100
    float mem_mb;
    float mem_limit_mb;
    bool gpu_passthrough;
    bool network_enabled;
    std::string image;
    std::string created;
    int pid;
};

struct SandboxState {
    std::vector<SandboxEntry> sandboxes;
    int selected_index = -1;

    // Create dialog
    bool show_create_dialog = false;
    char new_name[128] = {};
    bool new_gpu = false;
    bool new_network = true;
    float new_memory = 2048.0f;
    int new_image_idx = 0;

    // Snapshot / export
    bool show_snapshot_dialog = false;
    bool show_export_dialog = false;
    char snapshot_name[128] = {};
    char export_path[512] = {};

    // Delete
    bool show_delete_confirm = false;

    void init() {
        sandboxes.push_back({"dev-environment", "running", 23.5f, 45.2f, 1844.0f, 4096.0f, false, true, "straylight/dev:latest", "2026-03-15 09:00", 4521});
        sandboxes.push_back({"ml-training", "running", 87.3f, 72.8f, 5958.0f, 8192.0f, true, true, "straylight/ml:cuda12", "2026-03-14 14:00", 4890});
        sandboxes.push_back({"web-server", "running", 5.1f, 18.4f, 376.0f, 2048.0f, false, true, "straylight/web:nginx", "2026-03-12 08:00", 3201});
        sandboxes.push_back({"database-test", "stopped", 0.0f, 0.0f, 0.0f, 4096.0f, false, true, "straylight/db:postgres15", "2026-03-10 16:00", 0});
        sandboxes.push_back({"security-lab", "paused", 0.0f, 34.5f, 1408.0f, 4096.0f, false, false, "straylight/security:latest", "2026-03-13 11:00", 5102});
        sandboxes.push_back({"build-farm", "running", 45.0f, 55.0f, 2252.0f, 4096.0f, false, true, "straylight/build:gcc13", "2026-03-15 07:00", 5544});
    }
};

inline void render_sandbox_panel(SandboxState& st) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.67f, 1.0f));
    ImGui::Text("STRAYLIGHT SANDBOX");
    ImGui::PopStyleColor();
    ImGui::SameLine(ImGui::GetWindowWidth() - 80);
    if (ImGui::SmallButton("Close")) {}
    ImGui::Separator();
    ImGui::Spacing();

    // Toolbar
    if (ImGui::Button("Create Sandbox", ImVec2(140, 30))) {
        st.show_create_dialog = true;
        memset(st.new_name, 0, sizeof(st.new_name));
        st.new_gpu = false;
        st.new_network = true;
        st.new_memory = 2048.0f;
    }
    ImGui::SameLine();
    bool has_sel = st.selected_index >= 0 && st.selected_index < (int)st.sandboxes.size();
    if (!has_sel) ImGui::BeginDisabled();
    bool is_running = has_sel && st.sandboxes[st.selected_index].status == "running";
    if (is_running) {
        if (ImGui::Button("Stop", ImVec2(80, 30))) {
            st.sandboxes[st.selected_index].status = "stopped";
            st.sandboxes[st.selected_index].cpu_usage = 0;
            st.sandboxes[st.selected_index].pid = 0;
        }
    } else {
        if (ImGui::Button("Start", ImVec2(80, 30))) {
            if (has_sel) {
                st.sandboxes[st.selected_index].status = "running";
                st.sandboxes[st.selected_index].cpu_usage = 5.0f;
                st.sandboxes[st.selected_index].mem_usage = 20.0f;
                st.sandboxes[st.selected_index].mem_mb = st.sandboxes[st.selected_index].mem_limit_mb * 0.2f;
                st.sandboxes[st.selected_index].pid = 6000 + st.selected_index;
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Enter Terminal", ImVec2(120, 30))) {
        // Would launch terminal attached to sandbox
    }
    ImGui::SameLine();
    if (ImGui::Button("Snapshot", ImVec2(100, 30))) {
        st.show_snapshot_dialog = true;
        memset(st.snapshot_name, 0, sizeof(st.snapshot_name));
    }
    ImGui::SameLine();
    if (ImGui::Button("Export", ImVec2(80, 30))) {
        st.show_export_dialog = true;
        memset(st.export_path, 0, sizeof(st.export_path));
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.1f, 0.1f, 0.8f));
    if (ImGui::Button("Delete", ImVec2(80, 30))) { st.show_delete_confirm = true; }
    ImGui::PopStyleColor();
    if (!has_sel) ImGui::EndDisabled();

    ImGui::Spacing();

    // Sandbox list
    float list_w = ImGui::GetContentRegionAvail().x * 0.5f;
    if (ImGui::BeginChild("##sbox_list", ImVec2(list_w, -1), true)) {
        for (int i = 0; i < (int)st.sandboxes.size(); ++i) {
            auto& sb = st.sandboxes[i];
            ImGui::PushID(i);

            bool sel = (i == st.selected_index);
            if (ImGui::Selectable("##sel", sel, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, 70))) {
                st.selected_index = i;
            }

            // Overlay content on selectable
            ImVec2 p = ImGui::GetItemRectMin();
            ImDrawList* draw = ImGui::GetWindowDrawList();

            // Status indicator
            ImU32 status_col = sb.status == "running" ? IM_COL32(0, 200, 130, 255)
                             : sb.status == "paused"  ? IM_COL32(200, 180, 40, 255)
                             : IM_COL32(120, 120, 120, 255);
            draw->AddCircleFilled(ImVec2(p.x + 14, p.y + 20), 5.0f, status_col);

            // Name
            draw->AddText(ImVec2(p.x + 28, p.y + 8), IM_COL32(220, 220, 220, 255), sb.name.c_str());

            // Status text
            char status_text[64];
            snprintf(status_text, sizeof(status_text), "%s  |  PID: %d", sb.status.c_str(), sb.pid);
            draw->AddText(ImVec2(p.x + 28, p.y + 28), IM_COL32(140, 140, 140, 255), status_text);

            // CPU/Memory bars
            if (sb.status == "running" || sb.status == "paused") {
                float bar_x = p.x + 28;
                float bar_y = p.y + 48;
                float bar_w = list_w - 60;
                float bar_h = 8;

                // CPU bar
                draw->AddRectFilled(ImVec2(bar_x, bar_y), ImVec2(bar_x + bar_w, bar_y + bar_h), IM_COL32(30, 30, 50, 255));
                float cpu_w = bar_w * (sb.cpu_usage / 100.0f);
                ImU32 cpu_col = sb.cpu_usage < 50 ? IM_COL32(0, 180, 120, 255)
                              : sb.cpu_usage < 80 ? IM_COL32(200, 180, 40, 255)
                              : IM_COL32(200, 60, 60, 255);
                draw->AddRectFilled(ImVec2(bar_x, bar_y), ImVec2(bar_x + cpu_w, bar_y + bar_h), cpu_col);

                // Memory bar
                bar_y += bar_h + 2;
                draw->AddRectFilled(ImVec2(bar_x, bar_y), ImVec2(bar_x + bar_w, bar_y + bar_h), IM_COL32(30, 30, 50, 255));
                float mem_w = bar_w * (sb.mem_usage / 100.0f);
                ImU32 mem_col = sb.mem_usage < 50 ? IM_COL32(60, 120, 200, 255)
                              : sb.mem_usage < 80 ? IM_COL32(200, 180, 40, 255)
                              : IM_COL32(200, 60, 60, 255);
                draw->AddRectFilled(ImVec2(bar_x, bar_y), ImVec2(bar_x + mem_w, bar_y + bar_h), mem_col);
            }

            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Detail panel
    if (ImGui::BeginChild("##sbox_detail", ImVec2(0, -1), true)) {
        if (has_sel) {
            auto& sb = st.sandboxes[st.selected_index];
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.67f, 1.0f), "%s", sb.name.c_str());
            ImGui::Separator();
            ImGui::Spacing();

            ImVec4 status_col = sb.status == "running" ? ImVec4(0.2f, 1.0f, 0.5f, 1.0f)
                              : sb.status == "paused"  ? ImVec4(1.0f, 0.8f, 0.2f, 1.0f)
                              : ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
            ImGui::TextColored(status_col, "Status: %s", sb.status.c_str());
            ImGui::Text("Image:   %s", sb.image.c_str());
            ImGui::Text("Created: %s", sb.created.c_str());
            if (sb.pid > 0) ImGui::Text("PID:     %d", sb.pid);

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Configuration
            ImGui::Text("Configuration:");
            ImGui::BulletText("GPU Passthrough: %s", sb.gpu_passthrough ? "Enabled" : "Disabled");
            ImGui::BulletText("Network: %s", sb.network_enabled ? "Enabled" : "Isolated");
            ImGui::BulletText("Memory Limit: %.0f MB", sb.mem_limit_mb);

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Resource usage
            ImGui::Text("Resource Usage:");
            ImGui::Spacing();

            // CPU gauge
            ImGui::Text("CPU:");
            ImGui::SameLine(80);
            char cpu_label[32];
            snprintf(cpu_label, sizeof(cpu_label), "%.1f%%", sb.cpu_usage);
            ImGui::ProgressBar(sb.cpu_usage / 100.0f, ImVec2(-1, 20), cpu_label);

            // Memory gauge
            ImGui::Text("Memory:");
            ImGui::SameLine(80);
            char mem_label[64];
            snprintf(mem_label, sizeof(mem_label), "%.0f / %.0f MB (%.1f%%)", sb.mem_mb, sb.mem_limit_mb, sb.mem_usage);
            ImGui::ProgressBar(sb.mem_usage / 100.0f, ImVec2(-1, 20), mem_label);

            if (sb.gpu_passthrough) {
                ImGui::Spacing();
                ImGui::Text("GPU:");
                ImGui::SameLine(80);
                float gpu_usage = sb.status == "running" ? 65.0f : 0.0f;
                char gpu_label[32];
                snprintf(gpu_label, sizeof(gpu_label), "%.0f%%", gpu_usage);
                ImGui::ProgressBar(gpu_usage / 100.0f, ImVec2(-1, 20), gpu_label);
            }
        } else {
            ImGui::TextDisabled("Select a sandbox to view details");
        }
    }
    ImGui::EndChild();

    // Create dialog
    if (st.show_create_dialog) {
        ImGui::OpenPopup("Create Sandbox");
        st.show_create_dialog = false;
    }
    if (ImGui::BeginPopupModal("Create Sandbox", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Sandbox Name:");
        ImGui::SetNextItemWidth(300);
        ImGui::InputText("##sbox_name", st.new_name, sizeof(st.new_name));
        ImGui::Spacing();

        const char* images[] = {"straylight/dev:latest", "straylight/ml:cuda12", "straylight/web:nginx",
                                "straylight/db:postgres15", "straylight/security:latest", "straylight/build:gcc13"};
        ImGui::SetNextItemWidth(300);
        ImGui::Combo("Image", &st.new_image_idx, images, 6);
        ImGui::Spacing();

        ImGui::Checkbox("GPU Passthrough", &st.new_gpu);
        ImGui::Checkbox("Network Access", &st.new_network);
        ImGui::Spacing();

        ImGui::Text("Memory Limit (MB):");
        ImGui::SetNextItemWidth(300);
        ImGui::SliderFloat("##mem_slider", &st.new_memory, 256.0f, 32768.0f, "%.0f MB", ImGuiSliderFlags_Logarithmic);
        ImGui::Spacing();

        if (ImGui::Button("Create", ImVec2(120, 30))) {
            if (strlen(st.new_name) > 0) {
                SandboxEntry e;
                e.name = st.new_name;
                e.status = "stopped";
                e.cpu_usage = 0;
                e.mem_usage = 0;
                e.mem_mb = 0;
                e.mem_limit_mb = st.new_memory;
                e.gpu_passthrough = st.new_gpu;
                e.network_enabled = st.new_network;
                e.image = images[st.new_image_idx];
                e.created = "2026-03-16 12:00";
                e.pid = 0;
                st.sandboxes.push_back(e);
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 30))) { ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    // Snapshot dialog
    if (st.show_snapshot_dialog) {
        ImGui::OpenPopup("Sandbox Snapshot");
        st.show_snapshot_dialog = false;
    }
    if (ImGui::BeginPopupModal("Sandbox Snapshot", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Snapshot Name:");
        ImGui::SetNextItemWidth(300);
        ImGui::InputText("##snap_name", st.snapshot_name, sizeof(st.snapshot_name));
        ImGui::Spacing();
        if (ImGui::Button("Create Snapshot", ImVec2(150, 30))) { ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 30))) { ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    // Export dialog
    if (st.show_export_dialog) {
        ImGui::OpenPopup("Export Sandbox");
        st.show_export_dialog = false;
    }
    if (ImGui::BeginPopupModal("Export Sandbox", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Export to:");
        ImGui::SetNextItemWidth(400);
        ImGui::InputTextWithHint("##export_path", "/path/to/export.tar.gz", st.export_path, sizeof(st.export_path));
        ImGui::Spacing();
        if (ImGui::Button("Export", ImVec2(120, 30))) { ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 30))) { ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    // Delete confirmation
    if (st.show_delete_confirm) {
        ImGui::OpenPopup("Confirm Delete##sbox");
        st.show_delete_confirm = false;
    }
    if (ImGui::BeginPopupModal("Confirm Delete##sbox", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (has_sel) {
            ImGui::Text("Delete sandbox '%s'?", st.sandboxes[st.selected_index].name.c_str());
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "All data inside will be lost.");
        }
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.1f, 0.1f, 0.8f));
        if (ImGui::Button("Delete", ImVec2(120, 30))) {
            if (has_sel) {
                st.sandboxes.erase(st.sandboxes.begin() + st.selected_index);
                st.selected_index = -1;
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 30))) { ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

} // namespace straylight::sandbox
