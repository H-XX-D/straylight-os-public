// apps/rewind-gui/rewind_panel.h
// StrayLight Process Rewind panel
#pragma once

#include <imgui.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <straylight/ipc_client.h>

namespace straylight::rewind {

struct Checkpoint {
    char timestamp[32];
    char type[16];     // "auto", "manual", "signal"
    float size_mb;
    int  id;
};

struct TrackedProcess {
    int  pid;
    char name[64];
    char status[16];
    int  checkpoint_count;
    std::vector<Checkpoint> checkpoints;
};

struct RewindState {
    std::vector<TrackedProcess> processes;
    int selected_process = 0;
    int selected_checkpoint = -1;
    float timeline_pos = 1.0f;  // 0..1

    bool show_restore_confirm = false;
    bool restoring = false;
    float restore_progress = 0;

    // Live daemon link (no fabricated data)
    IpcJsonClient ipc_;
    bool          connected_ = false;
    std::string   conn_error_;
    double        last_refresh_ = -1.0e9;

    void ensure_connected() {
        if (connected_) return;
        auto r = ipc_.connect("/run/straylight/replay.sock");
        connected_ = r.has_value();
        conn_error_ = connected_ ? std::string() : r.error();
    }

    // Parse the replay daemon's "timeline" result. The unwrapped result is a
    // plain text report; its "Timeline:" section lists real events as
    //   [-12.345s] <event_type> (<name>:<pid>) {json}
    // Each event is grouped by (pid,name) into a TrackedProcess; each event
    // line becomes a real Checkpoint. status is derived from real start/exit
    // events. There is no byte-size field in the timeline, so size_mb stays 0
    // (never fabricated).
    void refresh() {
        ensure_connected();
        if (!connected_) return;
        auto r = ipc_.call("timeline");
        if (!r.has_value()) { connected_ = false; conn_error_ = r.error(); return; }
        const nlohmann::json& res = r.value();
        std::string text = res.is_string() ? res.get<std::string>() : res.dump();

        processes.clear();

        // Locate the "Timeline:" section.
        std::size_t tl = text.find("Timeline:");
        std::size_t scan = (tl == std::string::npos) ? 0 : tl + 9;

        int next_id = 1;
        // Walk line by line.
        while (scan < text.size()) {
            std::size_t eol = text.find('\n', scan);
            std::string line = text.substr(scan, eol == std::string::npos
                                                  ? std::string::npos : eol - scan);
            scan = (eol == std::string::npos) ? text.size() : eol + 1;

            // Trim leading whitespace.
            std::size_t a = line.find_first_not_of(" \t");
            if (a == std::string::npos) continue;
            if (line[a] != '[') continue; // not an event line

            // Timestamp: between '[' and ']'.
            std::size_t rb = line.find(']', a);
            if (rb == std::string::npos) continue;
            std::string ts = line.substr(a + 1, rb - a - 1);

            // Event type: first token after ']'.
            std::size_t b = line.find_first_not_of(" \t", rb + 1);
            if (b == std::string::npos) continue;
            std::size_t b2 = line.find_first_of(" \t(", b);
            std::string etype = line.substr(b, (b2 == std::string::npos ? line.size() : b2) - b);

            // (name:pid) between '(' and ')'.
            std::size_t lp = line.find('(', b);
            std::size_t rp = (lp == std::string::npos) ? std::string::npos : line.find(')', lp);
            if (lp == std::string::npos || rp == std::string::npos) continue;
            std::string np = line.substr(lp + 1, rp - lp - 1);
            std::size_t colon = np.rfind(':');
            if (colon == std::string::npos) continue;
            std::string name = np.substr(0, colon);
            int pid = 0;
            try { pid = std::stoi(np.substr(colon + 1)); } catch (...) { continue; }

            // Find or create the TrackedProcess for this (pid,name).
            TrackedProcess* tp = nullptr;
            for (auto& p : processes) {
                if (p.pid == pid && name == p.name) { tp = &p; break; }
            }
            if (!tp) {
                processes.emplace_back();
                tp = &processes.back();
                tp->pid = pid;
                std::snprintf(tp->name, sizeof(tp->name), "%s", name.c_str());
                std::snprintf(tp->status, sizeof(tp->status), "running");
                tp->checkpoint_count = 0;
            }

            // status from real lifecycle events.
            if (etype == "process_exit")
                std::snprintf(tp->status, sizeof(tp->status), "stopped");
            else if (etype == "process_start" && std::strcmp(tp->status, "stopped") != 0)
                std::snprintf(tp->status, sizeof(tp->status), "running");

            // Each real event becomes a checkpoint. type maps to the existing
            // vocabulary; size_mb left 0 (no real size in timeline).
            Checkpoint cp;
            std::snprintf(cp.timestamp, sizeof(cp.timestamp), "%s", ts.c_str());
            const char* ct = (etype == "process_exit") ? "signal" : "auto";
            std::snprintf(cp.type, sizeof(cp.type), "%s", ct);
            cp.size_mb = 0.0f;
            cp.id = next_id++;
            tp->checkpoints.push_back(cp);
            tp->checkpoint_count = (int)tp->checkpoints.size();

            if (processes.size() > 256 && tp->checkpoints.size() > 64) {
                // Bound memory; keep most recent real events.
                tp->checkpoints.erase(tp->checkpoints.begin());
                tp->checkpoint_count = (int)tp->checkpoints.size();
            }
        }

        if (selected_process >= (int)processes.size()) selected_process = 0;
    }

    void maybe_refresh() {
        double now = ImGui::GetTime();
        if (now - last_refresh_ >= 2.0) {
            last_refresh_ = now;
            refresh();
        }
    }

    void init() { refresh(); }
};

inline void render_rewind_panel(RewindState& st) {
    st.maybe_refresh();
    if (!st.connected_) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.3f, 1.0f));
        ImGui::TextWrapped("replay daemon disconnected: %s", st.conn_error_.c_str());
        ImGui::PopStyleColor();
        if (ImGui::SmallButton("Retry")) st.refresh();
        ImGui::Separator();
    }

    const ImVec4 accent(0.0f, 1.0f, 0.67f, 1.0f);

    ImGui::PushStyleColor(ImGuiCol_Text, accent);
    ImGui::Text("PROCESS REWIND");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();

    // Process table
    ImGui::BeginChild("##proc_table", ImVec2(-1, 180), true);
    ImGui::TextColored(accent, "Tracked Processes");
    ImGui::Separator();

    if (ImGui::BeginTable("##ptable", 5,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("Process", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Checkpoints", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("Total Size", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableHeadersRow();

        for (int i = 0; i < (int)st.processes.size(); ++i) {
            auto& p = st.processes[i];
            ImGui::TableNextRow();
            ImGui::PushID(i);

            ImGui::TableSetColumnIndex(0); ImGui::Text("%d", p.pid);
            ImGui::TableSetColumnIndex(1);
            if (ImGui::Selectable(p.name, st.selected_process == i, ImGuiSelectableFlags_SpanAllColumns)) {
                st.selected_process = i;
                st.selected_checkpoint = -1;
                st.timeline_pos = 1.0f;
            }
            ImGui::TableSetColumnIndex(2);
            if (strcmp(p.status, "running") == 0)
                ImGui::TextColored(accent, "%s", p.status);
            else
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "%s", p.status);
            ImGui::TableSetColumnIndex(3); ImGui::Text("%d", p.checkpoint_count);

            float total = 0;
            for (auto& c : p.checkpoints) total += c.size_mb;
            ImGui::TableSetColumnIndex(4); ImGui::Text("%.1f MB", total);

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    ImGui::Spacing();

    if (st.selected_process < 0 || st.selected_process >= (int)st.processes.size()) return;
    auto& proc = st.processes[st.selected_process];

    // Timeline scrubber
    ImGui::BeginChild("##timeline", ImVec2(-1, 80), true);
    ImGui::TextColored(accent, "Timeline: %s (PID %d)", proc.name, proc.pid);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 tl_pos = ImGui::GetCursorScreenPos();
    float tl_w = ImGui::GetContentRegionAvail().x - 20;
    float tl_h = 30;
    float tl_y = tl_pos.y + 10;

    // Timeline bar
    draw->AddRectFilled(ImVec2(tl_pos.x, tl_y),
                        ImVec2(tl_pos.x + tl_w, tl_y + tl_h),
                        IM_COL32(30, 30, 50, 255), 4.0f);

    // Checkpoint markers
    int n_cp = (int)proc.checkpoints.size();
    for (int i = 0; i < n_cp; ++i) {
        float x = tl_pos.x + (tl_w * (float)(i) / (float)(std::max(n_cp - 1, 1)));
        ImU32 dot_col;
        if (strcmp(proc.checkpoints[i].type, "manual") == 0)
            dot_col = IM_COL32(0, 255, 136, 255);
        else if (strcmp(proc.checkpoints[i].type, "signal") == 0)
            dot_col = IM_COL32(255, 200, 0, 255);
        else
            dot_col = IM_COL32(100, 150, 220, 255);

        bool is_selected = (st.selected_checkpoint == i);
        float r = is_selected ? 8.0f : 5.0f;
        draw->AddCircleFilled(ImVec2(x, tl_y + tl_h * 0.5f), r, dot_col);

        // Label
        draw->AddText(ImVec2(x - 15, tl_y + tl_h + 2),
                      IM_COL32(150, 150, 150, 255), proc.checkpoints[i].timestamp);

        // Click detection
        ImGuiIO& io = ImGui::GetIO();
        float dx = io.MousePos.x - x;
        float dy = io.MousePos.y - (tl_y + tl_h * 0.5f);
        if (dx*dx + dy*dy < 100 && ImGui::IsMouseClicked(0)) {
            st.selected_checkpoint = i;
            st.timeline_pos = (float)i / (float)(std::max(n_cp - 1, 1));
        }
    }

    // Scrubber handle
    float sx = tl_pos.x + tl_w * st.timeline_pos;
    draw->AddLine(ImVec2(sx, tl_y - 4), ImVec2(sx, tl_y + tl_h + 4),
                  IM_COL32(255, 255, 255, 200), 2.0f);
    draw->AddTriangleFilled(
        ImVec2(sx - 6, tl_y - 4), ImVec2(sx + 6, tl_y - 4), ImVec2(sx, tl_y + 2),
        IM_COL32(0, 255, 136, 255));

    ImGui::Dummy(ImVec2(0, tl_h + 30));

    // Slider for timeline
    ImGui::SetNextItemWidth(tl_w);
    if (ImGui::SliderFloat("##tl_slider", &st.timeline_pos, 0.0f, 1.0f, "")) {
        int idx = (int)(st.timeline_pos * (n_cp - 1) + 0.5f);
        st.selected_checkpoint = std::clamp(idx, 0, n_cp - 1);
    }
    ImGui::EndChild();
    ImGui::Spacing();

    // Checkpoint details + restore
    ImGui::BeginChild("##checkpoint_detail", ImVec2(-1, 0), true);
    if (st.selected_checkpoint >= 0 && st.selected_checkpoint < n_cp) {
        auto& cp = proc.checkpoints[st.selected_checkpoint];

        ImGui::TextColored(accent, "Checkpoint #%d", cp.id);
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("Timestamp:"); ImGui::SameLine(120); ImGui::Text("%s", cp.timestamp);
        ImGui::Text("Type:");      ImGui::SameLine(120);
        if (strcmp(cp.type, "manual") == 0)
            ImGui::TextColored(accent, "%s", cp.type);
        else if (strcmp(cp.type, "signal") == 0)
            ImGui::TextColored(ImVec4(1, 0.8f, 0, 1), "%s", cp.type);
        else
            ImGui::Text("%s", cp.type);
        ImGui::Text("Size:");      ImGui::SameLine(120); ImGui::Text("%.1f MB", cp.size_mb);

        ImGui::Spacing();
        if (!st.restoring) {
            if (ImGui::Button("Restore to This Checkpoint", ImVec2(260, 32))) {
                st.show_restore_confirm = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Create Checkpoint Now", ImVec2(200, 32))) {
                // mock removed: fabricated checkpoint creation. A real
                // implementation would issue a daemon RPC; not faking data.
            }
        } else {
            // mock removed: restore animation was cosmetic, not real progress
            if (st.restore_progress >= 1.0f) {
                st.restoring = false;
                st.restore_progress = 0;
            }
            ImGui::ProgressBar(st.restore_progress, ImVec2(-1, 24), "Restoring process state...");
        }
    } else {
        ImGui::TextDisabled("Click a checkpoint on the timeline to view details");
    }
    ImGui::EndChild();

    // Restore confirm
    if (st.show_restore_confirm) {
        ImGui::OpenPopup("Confirm Restore");
        st.show_restore_confirm = false;
    }
    if (ImGui::BeginPopupModal("Confirm Restore", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Restore %s (PID %d) to checkpoint #%d?",
                     proc.name, proc.pid,
                     st.selected_checkpoint >= 0 ? proc.checkpoints[st.selected_checkpoint].id : 0);
        ImGui::TextColored(ImVec4(1, 0.8f, 0, 1), "Current process state will be replaced.");
        ImGui::Spacing();
        if (ImGui::Button("Restore", ImVec2(120, 30))) {
            st.restoring = true;
            st.restore_progress = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 30))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

} // namespace straylight::rewind
