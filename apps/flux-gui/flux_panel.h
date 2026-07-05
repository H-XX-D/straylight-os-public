// apps/flux-gui/flux_panel.h
// StrayLight Flux GUI — Stream Monitor panel
#pragma once

#include <imgui.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <straylight/ipc_client.h>

namespace straylight::flux {

struct StreamEntry {
    std::string name;
    int         subscriber_count;
    float       message_rate;   // msgs/sec
    float       buffer_usage;   // 0-100
    int         total_messages;
    std::vector<std::string> recent_messages;
    float       rate_history[60] = {};
    int         rate_offset = 0;
};

struct FluxState {
    std::vector<StreamEntry> streams;
    int selected_index = -1;

    bool show_create_dialog = false;
    char new_name[128] = {};
    char filter_expr[256] = {};
    char publish_topic[128] = {};
    char publish_payload[1024] = {};
    bool show_delete_confirm = false;

    // Live daemon link (no fabricated streams)
    IpcJsonClient ipc_;
    bool          connected_ = false;
    std::string   conn_error_;
    double        last_refresh_ = -1.0e9;

    void ensure_connected() {
        if (connected_) return;
        auto r = ipc_.connect("/run/straylight/flux.sock");
        connected_ = r.has_value();
        conn_error_ = connected_ ? std::string() : r.error();
    }

    StreamEntry* find_stream(const std::string& nm) {
        for (auto& s : streams) if (s.name == nm) return &s;
        return nullptr;
    }

    void refresh() {
        ensure_connected();
        if (!connected_) return;
        auto r = ipc_.call("list");
        if (!r.has_value()) { connected_ = false; conn_error_ = r.error(); return; }
        const nlohmann::json& arr = r.value();
        if (!arr.is_array()) return;
        double now = ImGui::GetTime();
        double dt = (last_refresh_ > 0.0) ? (now - last_refresh_) : 0.0;
        std::vector<std::string> seen;
        for (const auto& sj : arr) {
            std::string nm = sj.value("name", std::string());
            if (nm.empty()) continue;
            seen.push_back(nm);
            StreamEntry* s = find_stream(nm);
            if (!s) { streams.emplace_back(); s = &streams.back(); s->name = nm; }
            int total = sj.value("total_published", 0);
            s->subscriber_count = sj.value("subscriber_count", 0);
            if (dt > 0.01) {
                int delta = total - s->total_messages;
                if (delta < 0) delta = 0;
                s->message_rate = (float)(delta / dt);
            }
            s->total_messages = total;
            s->buffer_usage = 0.0f; // current fill not exposed by flux list
            s->rate_history[s->rate_offset] = s->message_rate;
            s->rate_offset = (s->rate_offset + 1) % 60;
        }
        for (size_t i = 0; i < streams.size();) {
            bool present = false;
            for (auto& n : seen) if (n == streams[i].name) { present = true; break; }
            if (!present) streams.erase(streams.begin() + (long)i); else ++i;
        }
        last_refresh_ = now;
    }

    void maybe_refresh() {
        double now = ImGui::GetTime();
        if (now - last_refresh_ >= 2.0) refresh();
    }

    void init() { refresh(); }
};

inline void render_flux_panel(FluxState& st) {
    st.maybe_refresh();
    if (!st.connected_) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.3f, 1.0f));
        ImGui::TextWrapped("flux daemon disconnected: %s", st.conn_error_.c_str());
        ImGui::PopStyleColor();
        if (ImGui::SmallButton("Retry")) st.refresh();
        ImGui::Separator();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.67f, 1.0f));
    ImGui::Text("STRAYLIGHT FLUX");
    ImGui::PopStyleColor();
    ImGui::SameLine(ImGui::GetWindowWidth() - 80);
    if (ImGui::SmallButton("Close")) {}
    ImGui::Separator();
    ImGui::Spacing();

    // Toolbar
    if (ImGui::Button("Create Stream", ImVec2(130, 28))) {
        st.show_create_dialog = true;
        memset(st.new_name, 0, sizeof(st.new_name));
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(300);
    ImGui::InputTextWithHint("##filter", "Filter expression (e.g. type=tcp_connect)", st.filter_expr, sizeof(st.filter_expr));

    ImGui::Spacing();

    // Stream list (left) + detail (right)
    float list_w = 300.0f;
    if (ImGui::BeginChild("##stream_list", ImVec2(list_w, -1), true)) {
        ImGui::TextDisabled("Streams (%zu)", st.streams.size());
        ImGui::Separator();

        for (int i = 0; i < (int)st.streams.size(); ++i) {
            auto& s = st.streams[i];
            ImGui::PushID(i);
            bool sel = (i == st.selected_index);

            if (ImGui::Selectable("##sel", sel, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, 60))) {
                st.selected_index = i;
            }

            ImVec2 p = ImGui::GetItemRectMin();
            ImDrawList* draw = ImGui::GetWindowDrawList();

            // Name
            draw->AddText(ImVec2(p.x + 8, p.y + 4), IM_COL32(220, 220, 220, 255), s.name.c_str());

            // Stats line
            char stats[128];
            snprintf(stats, sizeof(stats), "%d subs | %.1f msg/s | %d total",
                     s.subscriber_count, s.message_rate, s.total_messages);
            draw->AddText(ImVec2(p.x + 8, p.y + 22), IM_COL32(140, 140, 140, 255), stats);

            // Buffer usage bar
            float bar_x = p.x + 8;
            float bar_y = p.y + 42;
            float bar_w = list_w - 30;
            draw->AddRectFilled(ImVec2(bar_x, bar_y), ImVec2(bar_x + bar_w, bar_y + 8), IM_COL32(30, 30, 50, 255));
            float fill_w = bar_w * (s.buffer_usage / 100.0f);
            ImU32 buf_col = s.buffer_usage < 50 ? IM_COL32(0, 180, 120, 255)
                          : s.buffer_usage < 80 ? IM_COL32(200, 180, 40, 255)
                          : IM_COL32(200, 60, 60, 255);
            draw->AddRectFilled(ImVec2(bar_x, bar_y), ImVec2(bar_x + fill_w, bar_y + 8), buf_col);

            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Detail panel
    if (ImGui::BeginChild("##stream_detail", ImVec2(0, -1), false)) {
        bool has_sel = st.selected_index >= 0 && st.selected_index < (int)st.streams.size();
        if (has_sel) {
            auto& s = st.streams[st.selected_index];

            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.67f, 1.0f), "%s", s.name.c_str());
            ImGui::Separator();
            ImGui::Spacing();

            // Stats
            ImGui::Columns(3, "##stream_stats", false);
            ImGui::Text("Subscribers: %d", s.subscriber_count);
            ImGui::NextColumn();
            ImGui::Text("Rate: %.1f msg/s", s.message_rate);
            ImGui::NextColumn();
            ImGui::Text("Buffer: %.0f%%", s.buffer_usage);
            ImGui::Columns(1);
            ImGui::Spacing();

            // Throughput graph
            ImGui::Text("Throughput (msgs/sec):");
            ImGui::PlotLines("##rate_graph", s.rate_history, 60, s.rate_offset,
                             nullptr, 0.0f, s.message_rate * 2.0f, ImVec2(-1, 100));
            // Animate
            // rate_history is updated from real daemon data in FluxState::refresh()

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Live message viewer
            ImGui::Text("Recent Messages:");
            if (ImGui::BeginChild("##messages", ImVec2(-1, 200), true)) {
                for (auto& msg : s.recent_messages) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.8f, 0.7f, 1.0f));
                    ImGui::TextWrapped("%s", msg.c_str());
                    ImGui::PopStyleColor();
                    ImGui::Separator();
                }
            }
            ImGui::EndChild();

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Publish test message
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.67f, 1.0f), "Publish Test Message");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextMultiline("##publish_payload", st.publish_payload, sizeof(st.publish_payload),
                                       ImVec2(-1, 80));
            if (ImGui::Button("Publish", ImVec2(120, 28))) {
                if (strlen(st.publish_payload) > 0) {
                    s.recent_messages.insert(s.recent_messages.begin(), st.publish_payload);
                    if (s.recent_messages.size() > 20) s.recent_messages.pop_back();
                    s.total_messages++;
                    memset(st.publish_payload, 0, sizeof(st.publish_payload));
                }
            }

            // Delete button
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 80);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.1f, 0.1f, 0.8f));
            if (ImGui::Button("Delete Stream", ImVec2(0, 28))) { st.show_delete_confirm = true; }
            ImGui::PopStyleColor();

        } else {
            ImGui::TextDisabled("Select a stream to view details");
        }
    }
    ImGui::EndChild();

    // Create Stream dialog
    if (st.show_create_dialog) {
        ImGui::OpenPopup("Create Stream");
        st.show_create_dialog = false;
    }
    if (ImGui::BeginPopupModal("Create Stream", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Stream Name:");
        ImGui::SetNextItemWidth(300);
        ImGui::InputTextWithHint("##stream_name", "e.g. my.custom.stream", st.new_name, sizeof(st.new_name));
        ImGui::Spacing();
        if (ImGui::Button("Create", ImVec2(120, 30))) {
            if (strlen(st.new_name) > 0) {
                StreamEntry s;
                s.name = st.new_name;
                s.subscriber_count = 0;
                s.message_rate = 0;
                s.buffer_usage = 0;
                s.total_messages = 0;
                for (int i = 0; i < 60; ++i) s.rate_history[i] = 0;
                st.streams.push_back(s);
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 30))) { ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    // Delete confirmation
    if (st.show_delete_confirm) {
        ImGui::OpenPopup("Confirm Delete##flux");
        st.show_delete_confirm = false;
    }
    if (ImGui::BeginPopupModal("Confirm Delete##flux", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        bool has_sel2 = st.selected_index >= 0 && st.selected_index < (int)st.streams.size();
        if (has_sel2) {
            ImGui::Text("Delete stream '%s'?", st.streams[st.selected_index].name.c_str());
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "All buffered messages will be lost.");
        }
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.1f, 0.1f, 0.8f));
        if (ImGui::Button("Delete", ImVec2(120, 30))) {
            if (has_sel2) {
                st.streams.erase(st.streams.begin() + st.selected_index);
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

} // namespace straylight::flux
