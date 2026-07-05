// apps/replay-gui/timeline_panel.h
// StrayLight Replay GUI — Timeline and event viewer panel
#pragma once

#include "event_detail.h"
#include <imgui.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <straylight/ipc_client.h>

namespace straylight::replay {

struct TimelineState {
    std::vector<SystemEvent> events;
    std::vector<SystemEvent> filtered_events;
    int selected_index = -1;

    // Filters
    bool filter_process = true;
    bool filter_network = true;
    bool filter_filesystem = true;
    bool filter_crash = true;
    bool filter_security = true;
    bool filter_system = true;
    char filter_pid[32] = {};
    char filter_time_start[32] = {};
    char filter_time_end[32] = {};
    char search_text[256] = {};

    // Live mode
    bool live_mode = false;
    float live_timer = 0.0f;
    int next_event_id = 100;

    // Crash analysis
    bool crash_analysis = false;

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

    // Map a daemon event type ("process_start", "process_exit",
    // "network_connect", "dmesg", ...) onto the render's display categories.
    static std::string map_type(const std::string& t) {
        if (t.rfind("process", 0) == 0) return "process";
        if (t.rfind("network", 0) == 0) return "network";
        if (t.rfind("file", 0) == 0)    return "filesystem";
        if (t == "dmesg" || t.rfind("kernel", 0) == 0) return "system";
        if (t.find("crash") != std::string::npos) return "crash";
        if (t.find("security") != std::string::npos || t.find("audit") != std::string::npos) return "security";
        return "system";
    }

    // Parse one Timeline: line of the form:
    //   "  [-298.953s] process_start (procname:pid) {json...}"
    // into a SystemEvent built entirely from real fields. Returns false if the
    // line is not an event row. No values are invented; absent fields stay empty.
    bool parse_event_line(const std::string& line, int seq, SystemEvent& e) {
        size_t lb = line.find('[');
        size_t rb = line.find(']', lb == std::string::npos ? 0 : lb);
        if (lb == std::string::npos || rb == std::string::npos) return false;
        std::string ts = line.substr(lb + 1, rb - lb - 1);          // e.g. -298.953s

        size_t p = rb + 1;
        while (p < line.size() && line[p] == ' ') ++p;
        size_t sp = line.find(' ', p);
        if (sp == std::string::npos) return false;
        std::string raw_type = line.substr(p, sp - p);              // e.g. process_start

        // (procname:pid)
        size_t op = line.find('(', sp);
        size_t cp = line.find(')', op == std::string::npos ? sp : op);
        std::string pname; int pid = 0;
        if (op != std::string::npos && cp != std::string::npos && cp > op) {
            std::string inside = line.substr(op + 1, cp - op - 1);
            size_t colon = inside.rfind(':');
            if (colon != std::string::npos) {
                pname = inside.substr(0, colon);
                const std::string pidstr = inside.substr(colon + 1);
                pid = 0;
                for (char c : pidstr) { if (c >= '0' && c <= '9') pid = pid * 10 + (c - '0'); else break; }
            } else {
                pname = inside;
            }
        }

        // trailing JSON payload, if any
        std::string payload;
        size_t br = line.find('{', cp == std::string::npos ? 0 : cp);
        if (br != std::string::npos) payload = line.substr(br);

        e.id           = seq;                       // display index; daemon exposes no stable id
        e.timestamp    = ts;                        // real relative timestamp from daemon
        e.type         = map_type(raw_type);        // real type, mapped to render category
        e.severity     = (e.type == "crash") ? "critical" : "info"; // derived, never random
        e.pid          = pid;                       // real
        e.process_name = pname;                     // real
        e.summary      = raw_type + (pname.empty() ? std::string() : (" " + pname)); // real
        e.json_payload = payload;                   // real detail JSON
        e.related_ids  = {};                        // no real source -> empty
        return true;
    }

    void refresh() {
        ensure_connected();
        if (!connected_) return;
        auto r = ipc_.call("timeline");             // mandated daemon verb
        if (!r.has_value()) { connected_ = false; conn_error_ = r.error(); return; }
        const nlohmann::json& j = r.value();

        // The "timeline" result is an ASCII summary string. Extract the real
        // per-event rows from its "Timeline:" section. If the daemon ever
        // returns a structured object instead, j.value() reads stay safe.
        std::string text;
        if (j.is_string()) text = j.get<std::string>();
        else if (j.is_object()) text = j.value("text", std::string());

        std::vector<SystemEvent> parsed;
        size_t tl = text.find("\nTimeline:");
        size_t pos = (tl == std::string::npos) ? std::string::npos : text.find('\n', tl + 1);
        int seq = 0;
        while (pos != std::string::npos && pos < text.size()) {
            size_t eol = text.find('\n', pos + 1);
            std::string line = text.substr(pos + 1, (eol == std::string::npos ? text.size() : eol) - (pos + 1));
            pos = eol;
            if (line.find('[') == std::string::npos) continue;
            SystemEvent e;
            if (parse_event_line(line, seq, e)) { parsed.push_back(std::move(e)); ++seq; }
            if (parsed.size() >= 500) break;        // render caps display; keep window bounded
        }

        events.swap(parsed);
        if (selected_index >= (int)events.size()) selected_index = -1;
        apply_filter();
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

    void apply_filter() {
        filtered_events.clear();
        std::string search(search_text);
        std::string pid_filter(filter_pid);

        for (auto& e : events) {
            // Type filter
            if (e.type == "process" && !filter_process) continue;
            if (e.type == "network" && !filter_network) continue;
            if (e.type == "filesystem" && !filter_filesystem) continue;
            if (e.type == "crash" && !filter_crash) continue;
            if (e.type == "security" && !filter_security) continue;
            if (e.type == "system" && !filter_system) continue;

            // PID filter
            if (!pid_filter.empty()) {
                char pid_str[32];
                snprintf(pid_str, sizeof(pid_str), "%d", e.pid);
                if (std::string(pid_str) != pid_filter) continue;
            }

            // Search filter
            if (!search.empty()) {
                if (e.summary.find(search) == std::string::npos &&
                    e.json_payload.find(search) == std::string::npos &&
                    e.process_name.find(search) == std::string::npos) continue;
            }

            // Crash analysis mode: only show errors/crashes
            if (crash_analysis && e.severity != "error" && e.severity != "critical") continue;

            filtered_events.push_back(e);
        }
    }
};

inline void render_timeline_panel(TimelineState& st) {
    st.maybe_refresh();
    if (!st.connected_) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.3f, 1.0f));
        ImGui::TextWrapped("replay daemon disconnected: %s", st.conn_error_.c_str());
        ImGui::PopStyleColor();
        if (ImGui::SmallButton("Retry")) st.refresh();
        ImGui::Separator();
    }

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.67f, 1.0f));
    ImGui::Text("STRAYLIGHT REPLAY");
    ImGui::PopStyleColor();
    ImGui::SameLine(ImGui::GetWindowWidth() - 80);
    if (ImGui::SmallButton("Close")) {}
    ImGui::Separator();
    ImGui::Spacing();

    // Filter bar
    ImGui::Text("Filters:");
    ImGui::SameLine();
    bool changed = false;
    ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.7f, 0.5f, 1.0f, 1.0f));
    changed |= ImGui::Checkbox("Process", &st.filter_process);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.2f, 0.6f, 1.0f, 1.0f));
    changed |= ImGui::Checkbox("Network", &st.filter_network);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.2f, 0.8f, 0.5f, 1.0f));
    changed |= ImGui::Checkbox("Filesystem", &st.filter_filesystem);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(1.0f, 0.2f, 0.2f, 1.0f));
    changed |= ImGui::Checkbox("Crash", &st.filter_crash);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(1.0f, 0.6f, 0.2f, 1.0f));
    changed |= ImGui::Checkbox("Security", &st.filter_security);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.6f, 0.6f, 0.8f, 1.0f));
    changed |= ImGui::Checkbox("System", &st.filter_system);
    ImGui::PopStyleColor();

    // PID filter
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    changed |= ImGui::InputTextWithHint("##pid_filter", "PID", st.filter_pid, sizeof(st.filter_pid));

    // Search
    ImGui::SetNextItemWidth(300);
    changed |= ImGui::InputTextWithHint("##search", "Search events...", st.search_text, sizeof(st.search_text));
    ImGui::SameLine();

    // Crash analysis toggle
    ImGui::PushStyleColor(ImGuiCol_Button, st.crash_analysis ? ImVec4(0.7f, 0.1f, 0.1f, 0.8f) : ImVec4(0.0f, 0.55f, 0.38f, 0.8f));
    if (ImGui::Button(st.crash_analysis ? "Crash Analysis: ON" : "Crash Analysis", ImVec2(160, 0))) {
        st.crash_analysis = !st.crash_analysis;
        changed = true;
    }
    ImGui::PopStyleColor();
    ImGui::SameLine();

    // Live mode toggle
    ImGui::PushStyleColor(ImGuiCol_Button, st.live_mode ? ImVec4(0.8f, 0.4f, 0.0f, 0.8f) : ImVec4(0.0f, 0.55f, 0.38f, 0.8f));
    if (ImGui::Button(st.live_mode ? "Live: ON" : "Live Mode", ImVec2(100, 0))) {
        st.live_mode = !st.live_mode;
    }
    ImGui::PopStyleColor();

    if (changed) st.apply_filter();

    // Live event injection removed: events now come from the replay daemon
    // via TimelineState::refresh()/maybe_refresh(); no synthetic injection.

    ImGui::Spacing();

    // Timeline visualization
    {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        float h = 50.0f;

        draw->AddRectFilled(ImVec2(p.x, p.y), ImVec2(p.x + w, p.y + h), IM_COL32(15, 15, 25, 255));
        float y_center = p.y + h * 0.5f;
        draw->AddLine(ImVec2(p.x + 10, y_center), ImVec2(p.x + w - 10, y_center), IM_COL32(60, 60, 100, 200), 1.0f);

        int n = (int)st.filtered_events.size();
        for (int i = 0; i < n && i < 200; ++i) {
            float x = p.x + 10 + (w - 20) * (float)i / (float)std::max(n - 1, 1);
            ImU32 col = type_color_u32(st.filtered_events[i].type);
            float r = (i == st.selected_index) ? 6.0f : 4.0f;
            draw->AddCircleFilled(ImVec2(x, y_center), r, col);
            if (st.filtered_events[i].severity == "critical" || st.filtered_events[i].severity == "error") {
                draw->AddCircle(ImVec2(x, y_center), r + 3.0f, IM_COL32(255, 80, 80, 180), 0, 1.5f);
            }
        }
        ImGui::Dummy(ImVec2(0, h));
    }

    ImGui::Spacing();

    // Event list + detail split
    float left_w = ImGui::GetContentRegionAvail().x * 0.5f;
    if (ImGui::BeginChild("##event_list", ImVec2(left_w, -1), true)) {
        ImGui::TextDisabled("%zu events", st.filtered_events.size());
        ImGui::Separator();

        for (int i = 0; i < (int)st.filtered_events.size(); ++i) {
            auto& e = st.filtered_events[i];
            ImGui::PushID(i);

            bool sel = (i == st.selected_index);
            ImVec4 sev_col = severity_color(e.severity);

            // Severity indicator
            ImGui::TextColored(sev_col, "[%s]", e.severity.c_str());
            ImGui::SameLine();

            char label[256];
            snprintf(label, sizeof(label), "%s | %s [%d] %s##ev%d",
                     e.timestamp.c_str(), e.type.c_str(), e.pid, e.summary.c_str(), e.id);
            if (ImGui::Selectable(label, sel)) {
                st.selected_index = i;
            }

            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Detail panel
    if (ImGui::BeginChild("##event_detail", ImVec2(0, -1), true)) {
        if (st.selected_index >= 0 && st.selected_index < (int)st.filtered_events.size()) {
            render_event_detail(st.filtered_events[st.selected_index]);
        } else {
            ImGui::TextDisabled("Select an event to view details");
        }
    }
    ImGui::EndChild();
}

} // namespace straylight::replay
