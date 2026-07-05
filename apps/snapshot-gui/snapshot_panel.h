// apps/snapshot-gui/snapshot_panel.h
// StrayLight Snapshot GUI — Snapshot Manager panel
#pragma once

#include <imgui.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <algorithm>

namespace straylight::snapshot {

struct SnapshotEntry {
    std::string name;
    std::string date;
    std::string size;
    std::string type; // "manual" or "auto"
    std::string description;
    std::vector<std::string> changed_files;
};

struct SnapshotState {
    std::vector<SnapshotEntry> snapshots;
    int selected_index = -1;

    // Create dialog
    bool show_create_dialog = false;
    char new_name[128] = {};
    char new_desc[512] = {};

    // Restore / delete confirmations
    bool show_restore_confirm = false;
    bool show_delete_confirm = false;

    // Schedule config
    bool auto_enabled = true;
    int  schedule_idx = 1; // 0=hourly, 1=daily, 2=weekly
    int  retention_days = 30;

    // Diff viewer
    bool show_diff = false;
    int  diff_snap_idx = -1;

    // Timeline
    bool show_timeline = false;

    void init() {
        snapshots.push_back({
            "pre-upgrade-kernel", "2026-03-15 22:00", "2.4 GB", "manual",
            "Before kernel 6.8 upgrade",
            {"/boot/vmlinuz-6.7.12", "/boot/initramfs-6.7.12.img", "/etc/default/grub"}
        });
        snapshots.push_back({
            "daily-2026-03-15", "2026-03-15 03:00", "1.8 GB", "auto",
            "Scheduled daily snapshot",
            {"/etc/straylight/flux.conf", "/var/log/straylight/health.log"}
        });
        snapshots.push_back({
            "post-ml-stack", "2026-03-14 18:30", "3.1 GB", "manual",
            "After ML stack deployment",
            {"/opt/straylight/ml/models/", "/etc/straylight/mesh.conf", "/usr/lib/libcuda.so.535.129"}
        });
        snapshots.push_back({
            "daily-2026-03-14", "2026-03-14 03:00", "1.7 GB", "auto",
            "Scheduled daily snapshot",
            {"/etc/passwd", "/etc/straylight/shield.conf"}
        });
        snapshots.push_back({
            "pre-network-reconfig", "2026-03-13 14:00", "2.0 GB", "manual",
            "Before network reconfiguration",
            {"/etc/netplan/01-straylight.yaml", "/etc/straylight/probe.conf", "/etc/resolv.conf"}
        });
        snapshots.push_back({
            "daily-2026-03-13", "2026-03-13 03:00", "1.6 GB", "auto",
            "Scheduled daily snapshot",
            {"/var/lib/straylight/cron.db"}
        });
        snapshots.push_back({
            "initial-install", "2026-03-10 12:00", "4.2 GB", "manual",
            "Initial StrayLight OS installation snapshot",
            {}
        });
    }
};

inline void render_snapshot_panel(SnapshotState& st) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.67f, 1.0f));
    ImGui::Text("STRAYLIGHT SNAPSHOT");
    ImGui::PopStyleColor();
    ImGui::SameLine(ImGui::GetWindowWidth() - 80);
    if (ImGui::SmallButton("Close")) {}
    ImGui::Separator();
    ImGui::Spacing();

    // Toolbar
    if (ImGui::Button("Create Snapshot", ImVec2(150, 30))) {
        st.show_create_dialog = true;
        memset(st.new_name, 0, sizeof(st.new_name));
        memset(st.new_desc, 0, sizeof(st.new_desc));
    }
    ImGui::SameLine();
    bool has_sel = st.selected_index >= 0 && st.selected_index < (int)st.snapshots.size();
    if (!has_sel) ImGui::BeginDisabled();
    if (ImGui::Button("Restore", ImVec2(100, 30))) { st.show_restore_confirm = true; }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.1f, 0.1f, 0.8f));
    if (ImGui::Button("Delete", ImVec2(100, 30))) { st.show_delete_confirm = true; }
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::Button("View Diff", ImVec2(100, 30))) {
        st.show_diff = true;
        st.diff_snap_idx = st.selected_index;
    }
    if (!has_sel) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Timeline", ImVec2(100, 30))) { st.show_timeline = !st.show_timeline; }

    ImGui::Spacing();

    float main_height = st.show_timeline ? ImGui::GetContentRegionAvail().y * 0.6f : ImGui::GetContentRegionAvail().y;

    // Snapshot table
    if (ImGui::BeginChild("##snap_list_area", ImVec2(0, main_height), false)) {
        float list_w = ImGui::GetContentRegionAvail().x * 0.55f;
        if (ImGui::BeginChild("##snap_table", ImVec2(list_w, -1), true)) {
            if (ImGui::BeginTable("##snaps", 4,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                    ImGuiTableFlags_Resizable)) {
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthFixed, 150);
                ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 70);
                ImGui::TableHeadersRow();
                for (int i = 0; i < (int)st.snapshots.size(); ++i) {
                    auto& s = st.snapshots[i];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    bool sel = (i == st.selected_index);
                    if (ImGui::Selectable(s.name.c_str(), sel, ImGuiSelectableFlags_SpanAllColumns)) {
                        st.selected_index = i;
                    }
                    ImGui::TableNextColumn();
                    ImGui::Text("%s", s.date.c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%s", s.size.c_str());
                    ImGui::TableNextColumn();
                    ImVec4 type_col = s.type == "manual" ? ImVec4(0.4f, 0.8f, 1.0f, 1.0f) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
                    ImGui::TextColored(type_col, "%s", s.type.c_str());
                }
                ImGui::EndTable();
            }
        }
        ImGui::EndChild();

        ImGui::SameLine();

        // Detail / diff panel
        if (ImGui::BeginChild("##snap_detail", ImVec2(0, -1), true)) {
            if (has_sel) {
                auto& s = st.snapshots[st.selected_index];
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.67f, 1.0f), "%s", s.name.c_str());
                ImGui::Separator();
                ImGui::Text("Date: %s", s.date.c_str());
                ImGui::Text("Size: %s", s.size.c_str());
                ImGui::Text("Type: %s", s.type.c_str());
                ImGui::Spacing();
                ImGui::TextWrapped("Description: %s", s.description.c_str());
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                if (st.show_diff && st.diff_snap_idx == st.selected_index) {
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Changed Files:");
                    ImGui::Spacing();
                    if (s.changed_files.empty()) {
                        ImGui::TextDisabled("  (base snapshot - no diff available)");
                    } else {
                        for (auto& f : s.changed_files) {
                            ImGui::BulletText("%s", f.c_str());
                        }
                    }
                } else {
                    ImGui::TextDisabled("Click 'View Diff' to see changed files");
                }

                // Schedule config
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.67f, 1.0f), "Auto-Snapshot Schedule");
                ImGui::Checkbox("Enable auto-snapshots", &st.auto_enabled);
                if (st.auto_enabled) {
                    const char* schedules[] = {"Hourly", "Daily", "Weekly"};
                    ImGui::SetNextItemWidth(150);
                    ImGui::Combo("Frequency", &st.schedule_idx, schedules, 3);
                    ImGui::SetNextItemWidth(150);
                    ImGui::SliderInt("Retention (days)", &st.retention_days, 1, 90);
                }
            } else {
                ImGui::TextDisabled("Select a snapshot to view details");
            }
        }
        ImGui::EndChild();
    }
    ImGui::EndChild();

    // Timeline view
    if (st.show_timeline) {
        ImGui::Separator();
        if (ImGui::BeginChild("##timeline", ImVec2(0, 0), true)) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.67f, 1.0f), "Snapshot Timeline");
            ImGui::Spacing();
            ImDrawList* draw = ImGui::GetWindowDrawList();
            ImVec2 p = ImGui::GetCursorScreenPos();
            float w = ImGui::GetContentRegionAvail().x;
            float y_center = p.y + 30;

            // Draw timeline line
            draw->AddLine(ImVec2(p.x + 20, y_center), ImVec2(p.x + w - 20, y_center),
                          IM_COL32(100, 100, 160, 200), 2.0f);

            int n = (int)st.snapshots.size();
            for (int i = 0; i < n; ++i) {
                float x = p.x + 20 + (w - 40) * (float)(n - 1 - i) / (float)std::max(n - 1, 1);
                ImU32 col = st.snapshots[i].type == "manual" ? IM_COL32(0, 200, 130, 255) : IM_COL32(100, 100, 200, 255);
                draw->AddCircleFilled(ImVec2(x, y_center), 8.0f, col);
                if (i == st.selected_index) {
                    draw->AddCircle(ImVec2(x, y_center), 12.0f, IM_COL32(255, 255, 255, 200), 0, 2.0f);
                }
                // Label below
                const char* label = st.snapshots[i].name.c_str();
                ImVec2 text_size = ImGui::CalcTextSize(label);
                draw->AddText(ImVec2(x - text_size.x * 0.5f, y_center + 14), IM_COL32(200, 200, 200, 200), label);
            }

            ImGui::Dummy(ImVec2(0, 70));
        }
        ImGui::EndChild();
    }

    // Create dialog
    if (st.show_create_dialog) {
        ImGui::OpenPopup("Create Snapshot");
        st.show_create_dialog = false;
    }
    if (ImGui::BeginPopupModal("Create Snapshot", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Snapshot Name:");
        ImGui::SetNextItemWidth(350);
        ImGui::InputText("##snap_name", st.new_name, sizeof(st.new_name));
        ImGui::Spacing();
        ImGui::Text("Description:");
        ImGui::InputTextMultiline("##snap_desc", st.new_desc, sizeof(st.new_desc), ImVec2(350, 80));
        ImGui::Spacing();
        if (ImGui::Button("Create", ImVec2(120, 30))) {
            if (strlen(st.new_name) > 0) {
                SnapshotEntry e;
                e.name = st.new_name;
                e.date = "2026-03-16 12:00";
                e.size = "0 B";
                e.type = "manual";
                e.description = st.new_desc;
                st.snapshots.insert(st.snapshots.begin(), e);
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 30))) { ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    // Restore confirmation
    if (st.show_restore_confirm) {
        ImGui::OpenPopup("Confirm Restore");
        st.show_restore_confirm = false;
    }
    if (ImGui::BeginPopupModal("Confirm Restore", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (has_sel) {
            ImGui::Text("Restore snapshot '%s'?", st.snapshots[st.selected_index].name.c_str());
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "This will revert your system to this snapshot.");
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "A backup of current state will be created first.");
        }
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.5f, 0.0f, 0.8f));
        if (ImGui::Button("Restore", ImVec2(120, 30))) { ImGui::CloseCurrentPopup(); }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 30))) { ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    // Delete confirmation
    if (st.show_delete_confirm) {
        ImGui::OpenPopup("Confirm Delete##snap");
        st.show_delete_confirm = false;
    }
    if (ImGui::BeginPopupModal("Confirm Delete##snap", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (has_sel) {
            ImGui::Text("Delete snapshot '%s'?", st.snapshots[st.selected_index].name.c_str());
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "This action cannot be undone.");
        }
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.1f, 0.1f, 0.8f));
        if (ImGui::Button("Delete", ImVec2(120, 30))) {
            if (has_sel) {
                st.snapshots.erase(st.snapshots.begin() + st.selected_index);
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

} // namespace straylight::snapshot
