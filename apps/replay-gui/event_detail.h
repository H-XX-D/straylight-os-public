// apps/replay-gui/event_detail.h
// StrayLight Replay GUI — Event detail rendering
#pragma once

#include <imgui.h>
#include <string>
#include <vector>

namespace straylight::replay {

struct SystemEvent {
    int         id;
    std::string timestamp;
    std::string type;      // "process", "network", "filesystem", "crash", "security", "system"
    std::string severity;  // "info", "warning", "error", "critical"
    int         pid;
    std::string process_name;
    std::string summary;
    std::string json_payload;
    std::vector<int> related_ids;
};

inline ImVec4 severity_color(const std::string& sev) {
    if (sev == "critical") return ImVec4(1.0f, 0.2f, 0.2f, 1.0f);
    if (sev == "error")    return ImVec4(1.0f, 0.5f, 0.3f, 1.0f);
    if (sev == "warning")  return ImVec4(1.0f, 0.8f, 0.2f, 1.0f);
    return ImVec4(0.6f, 0.8f, 0.6f, 1.0f); // info
}

inline ImU32 type_color_u32(const std::string& type) {
    if (type == "crash")      return IM_COL32(255, 60, 60, 255);
    if (type == "security")   return IM_COL32(255, 160, 40, 255);
    if (type == "network")    return IM_COL32(60, 160, 255, 255);
    if (type == "filesystem") return IM_COL32(60, 200, 130, 255);
    if (type == "process")    return IM_COL32(180, 120, 255, 255);
    return IM_COL32(140, 140, 200, 255); // system
}

inline void render_event_detail(const SystemEvent& ev) {
    ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.67f, 1.0f), "Event #%d", ev.id);
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Time:     %s", ev.timestamp.c_str());
    ImGui::Text("Type:     %s", ev.type.c_str());
    ImGui::Text("Severity:");
    ImGui::SameLine();
    ImGui::TextColored(severity_color(ev.severity), "%s", ev.severity.c_str());
    ImGui::Text("PID:      %d", ev.pid);
    ImGui::Text("Process:  %s", ev.process_name.c_str());
    ImGui::Spacing();
    ImGui::TextWrapped("Summary: %s", ev.summary.c_str());
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("JSON Payload:");
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.05f, 0.05f, 0.08f, 1.0f));
    char payload[2048];
    snprintf(payload, sizeof(payload), "%s", ev.json_payload.c_str());
    ImGui::InputTextMultiline("##json_payload", payload, sizeof(payload),
                               ImVec2(-1, 180), ImGuiInputTextFlags_ReadOnly);
    ImGui::PopStyleColor();

    if (!ev.related_ids.empty()) {
        ImGui::Spacing();
        ImGui::Text("Related Events:");
        for (int rid : ev.related_ids) {
            ImGui::SameLine();
            char label[32];
            snprintf(label, sizeof(label), "#%d", rid);
            ImGui::SmallButton(label);
        }
    }
}

} // namespace straylight::replay
