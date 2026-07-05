// apps/power-gui/power_panel.h
// StrayLight Power Settings panel
#pragma once

#include <imgui.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cmath>

namespace straylight::power {

struct PowerState {
    float battery_pct = 72.0f;
    bool  on_ac = true;
    int   power_profile = 1;  // 0=saver, 1=balanced, 2=performance
    float brightness = 0.75f;

    int   battery_health = 92;
    int   cycle_count = 245;
    float design_capacity = 72.0f;  // Wh
    float current_capacity = 66.24f;  // Wh
    float charge_rate = 45.0f;  // W

    int   estimated_hours = 4;
    int   estimated_mins = 32;

    int   low_battery_action = 1;  // 0=nothing, 1=notify, 2=suspend, 3=hibernate
    int   critical_battery_action = 2;
    int   low_battery_threshold = 15;
    int   critical_battery_threshold = 5;
    int   lid_close_action = 1;  // 0=nothing, 1=suspend, 2=hibernate, 3=shutdown

    static constexpr const char* profiles[] = { "Power Saver", "Balanced", "Performance" };
    static constexpr const char* battery_actions[] = { "Nothing", "Notify", "Suspend", "Hibernate", "Shutdown" };
    static constexpr const char* lid_actions[] = { "Nothing", "Suspend", "Hibernate", "Shutdown" };

    float anim_timer = 0;
};

inline void render_power_panel(PowerState& st) {
    const ImVec4 accent(0.0f, 1.0f, 0.67f, 1.0f);
    st.anim_timer += ImGui::GetIO().DeltaTime;

    ImGui::PushStyleColor(ImGuiCol_Text, accent);
    ImGui::Text("POWER SETTINGS");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();

    float left_w = ImGui::GetContentRegionAvail().x * 0.45f;

    // Battery status with arc gauge
    ImGui::BeginChild("##battery", ImVec2(left_w, ImGui::GetContentRegionAvail().y * 0.55f), true);
    ImGui::TextColored(accent, "Battery Status");
    ImGui::Separator();
    ImGui::Spacing();

    // Arc gauge
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 center_pos = ImGui::GetCursorScreenPos();
    float cx = center_pos.x + left_w * 0.5f - 12;
    float cy = center_pos.y + 90;
    float radius = 70;

    // Background arc
    float start_angle = 2.356f;  // ~135 degrees
    float end_angle = 7.069f;    // ~405 degrees (135+270)
    int segments = 60;
    for (int i = 0; i < segments; ++i) {
        float a1 = start_angle + (end_angle - start_angle) * i / segments;
        float a2 = start_angle + (end_angle - start_angle) * (i + 1) / segments;
        draw->AddLine(
            ImVec2(cx + cosf(a1) * radius, cy + sinf(a1) * radius),
            ImVec2(cx + cosf(a2) * radius, cy + sinf(a2) * radius),
            IM_COL32(40, 40, 60, 255), 8.0f);
    }

    // Filled arc
    int fill_segs = (int)(segments * st.battery_pct / 100.0f);
    ImU32 arc_col = (st.battery_pct > 50) ? IM_COL32(0, 255, 136, 255) :
                    (st.battery_pct > 20) ? IM_COL32(255, 200, 0, 255) :
                    IM_COL32(255, 60, 60, 255);
    for (int i = 0; i < fill_segs; ++i) {
        float a1 = start_angle + (end_angle - start_angle) * i / segments;
        float a2 = start_angle + (end_angle - start_angle) * (i + 1) / segments;
        draw->AddLine(
            ImVec2(cx + cosf(a1) * radius, cy + sinf(a1) * radius),
            ImVec2(cx + cosf(a2) * radius, cy + sinf(a2) * radius),
            arc_col, 8.0f);
    }

    // Center text
    char pct_str[16];
    snprintf(pct_str, 16, "%.0f%%", st.battery_pct);
    ImVec2 pct_size = ImGui::CalcTextSize(pct_str);
    draw->AddText(ImVec2(cx - pct_size.x * 0.5f, cy - 10), IM_COL32(255, 255, 255, 255), pct_str);

    const char* ac_str = st.on_ac ? "Charging" : "On Battery";
    ImVec2 ac_size = ImGui::CalcTextSize(ac_str);
    draw->AddText(ImVec2(cx - ac_size.x * 0.5f, cy + 10),
                  st.on_ac ? IM_COL32(0, 200, 100, 255) : IM_COL32(200, 200, 200, 255), ac_str);

    ImGui::Dummy(ImVec2(0, 180));

    // Time remaining
    ImGui::Text("Estimated Time:");
    ImGui::SameLine(140);
    if (st.on_ac) {
        ImGui::TextColored(accent, "Fully charged in ~1h 20m");
    } else {
        ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.9f, 1.0f), "%dh %dm remaining", st.estimated_hours, st.estimated_mins);
    }

    ImGui::Text("Charge Rate:");
    ImGui::SameLine(140);
    ImGui::Text("%.1f W", st.charge_rate);

    ImGui::EndChild();

    ImGui::SameLine();

    // Right: Power profile and brightness
    ImGui::BeginChild("##profile", ImVec2(0, ImGui::GetContentRegionAvail().y * 0.55f), true);
    ImGui::TextColored(accent, "Power Profile");
    ImGui::Separator();
    ImGui::Spacing();

    // Profile selector (3 cards)
    float card_w = (ImGui::GetContentRegionAvail().x - 16) / 3.0f;
    const char* profile_desc[] = {
        "Max battery\nReduced performance\nDim display",
        "Balanced\nAdaptive performance\nNormal brightness",
        "Max performance\nFull brightness\nGPU boost"
    };
    const char* profile_icons[] = {"[S]", "[B]", "[P]"};

    for (int i = 0; i < 3; ++i) {
        if (i > 0) ImGui::SameLine();
        ImGui::PushID(i);

        ImVec2 pos = ImGui::GetCursorScreenPos();
        bool selected = (st.power_profile == i);

        ImU32 bg = selected ? IM_COL32(0, 80, 55, 200) : IM_COL32(25, 25, 40, 200);
        ImU32 border = selected ? IM_COL32(0, 255, 136, 255) : IM_COL32(60, 60, 80, 255);

        draw->AddRectFilled(pos, ImVec2(pos.x + card_w, pos.y + 100), bg, 6.0f);
        draw->AddRect(pos, ImVec2(pos.x + card_w, pos.y + 100), border, 6.0f, 0, 2.0f);

        draw->AddText(ImVec2(pos.x + 8, pos.y + 8), IM_COL32(0, 255, 136, 255), profile_icons[i]);
        draw->AddText(ImVec2(pos.x + 8, pos.y + 28), IM_COL32(220, 220, 220, 255),
                      PowerState::profiles[i]);
        draw->AddText(ImVec2(pos.x + 8, pos.y + 50), IM_COL32(150, 150, 150, 220),
                      profile_desc[i]);

        if (ImGui::InvisibleButton("##prof", ImVec2(card_w, 100))) {
            st.power_profile = i;
        }

        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0, 4));

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Brightness slider
    ImGui::Text("Brightness:");
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##brightness", &st.brightness, 0.0f, 1.0f, "%.0f%%");

    // Visual brightness bar
    ImVec2 bpos = ImGui::GetCursorScreenPos();
    float bw = ImGui::GetContentRegionAvail().x;
    ImU32 dim_col = IM_COL32((int)(40 * st.brightness), (int)(40 * st.brightness),
                              (int)(60 * st.brightness), 255);
    ImU32 bright_col = IM_COL32((int)(255 * st.brightness), (int)(255 * st.brightness),
                                 (int)(200 * st.brightness), 255);
    draw->AddRectFilled(bpos, ImVec2(bpos.x + bw, bpos.y + 16), dim_col, 3.0f);
    draw->AddRectFilled(bpos, ImVec2(bpos.x + bw * st.brightness, bpos.y + 16), bright_col, 3.0f);
    ImGui::Dummy(ImVec2(0, 22));

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(accent, "Battery Health");
    ImGui::Spacing();
    ImGui::Text("Health:");          ImGui::SameLine(140); ImGui::Text("%d%%", st.battery_health);
    ImGui::Text("Cycle Count:");     ImGui::SameLine(140); ImGui::Text("%d", st.cycle_count);
    ImGui::Text("Design Capacity:"); ImGui::SameLine(140); ImGui::Text("%.1f Wh", st.design_capacity);
    ImGui::Text("Current Capacity:");ImGui::SameLine(140); ImGui::Text("%.1f Wh", st.current_capacity);

    ImGui::EndChild();

    // Bottom: Battery actions
    ImGui::BeginChild("##actions", ImVec2(-1, 0), true);
    ImGui::TextColored(accent, "Low Battery Actions");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Low Battery (%d%%):", st.low_battery_threshold);
    ImGui::SameLine(200);
    ImGui::SetNextItemWidth(200);
    ImGui::Combo("##low_action", &st.low_battery_action, PowerState::battery_actions, 5);
    ImGui::SameLine(440);
    ImGui::Text("Threshold:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    ImGui::InputInt("##low_thresh", &st.low_battery_threshold, 0);

    ImGui::Text("Critical Battery (%d%%):", st.critical_battery_threshold);
    ImGui::SameLine(200);
    ImGui::SetNextItemWidth(200);
    ImGui::Combo("##crit_action", &st.critical_battery_action, PowerState::battery_actions, 5);
    ImGui::SameLine(440);
    ImGui::Text("Threshold:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    ImGui::InputInt("##crit_thresh", &st.critical_battery_threshold, 0);

    ImGui::Text("Lid Close:");
    ImGui::SameLine(200);
    ImGui::SetNextItemWidth(200);
    ImGui::Combo("##lid_action", &st.lid_close_action, PowerState::lid_actions, 4);

    ImGui::EndChild();
}

} // namespace straylight::power
