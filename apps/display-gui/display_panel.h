// apps/display-gui/display_panel.h
// StrayLight Display Settings panel
#pragma once

#include <imgui.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace straylight::display {

struct MonitorInfo {
    char name[64];
    float x, y;          // position in layout (normalized 0-1)
    float w, h;          // size in layout
    int   res_index;     // current resolution index (into res_list)
    int   rate_index;    // current refresh rate index (no real sysfs source)
    bool  primary;
    bool  dragging;
    // Real data loaded from /sys/class/drm in DisplayState::refresh()
    std::string status;                 // "connected" / "disconnected"
    bool        enabled = false;        // /sys/class/drm/<c>/enabled
    std::vector<std::string> res_list;  // real modes (WxH), deduped
    std::vector<const char*> res_cstr;  // pointers into res_list for ImGui::Combo
};

// WIRED-OS: display-gui bound to /sys/class/drm
struct DisplayState {
    std::vector<MonitorInfo> monitors;
    int selected_monitor = 0;

    float color_temp = 6500.0f;  // Kelvin
    bool  night_mode = false;

    int   profile_index = 0;
    char  profile_name[128] = {};
    bool  show_save_dialog = false;

    // Refresh-rate list kept only as a UI fallback; sysfs /modes does not
    // expose refresh rate, so we never select a fabricated value.
    static constexpr const char* refresh_rates[] = {
        "144 Hz", "120 Hz", "90 Hz", "60 Hz", "50 Hz", "30 Hz"
    };
    static constexpr int num_rates = 6;

    static constexpr const char* profiles[] = {
        "Default", "Dual Landscape", "Portrait + Landscape", "Presentation"
    };
    static constexpr int num_profiles = 4;

    // OS-backed source status (no daemon / socket).
    bool        ok_ = false;
    std::string err_;
    double      last_refresh_ = -1.0e9;

    // No daemon to connect to; record that the sysfs source is reachable.
    void ensure_connected() {
        std::error_code ec;
        if (std::filesystem::exists("/sys/class/drm", ec) && !ec) {
            ok_ = true;
            err_.clear();
        } else {
            ok_ = false;
            err_ = "/sys/class/drm not available";
        }
    }

    // Read REAL connector data from /sys/class/drm into the SAME fields render reads.
    void refresh() {
        ensure_connected();
        namespace fs = std::filesystem;
        std::error_code ec;

        std::vector<MonitorInfo> next;
        std::vector<std::string> conns;
        for (const auto& entry : fs::directory_iterator("/sys/class/drm", ec)) {
            std::string n = entry.path().filename().string();
            // Connector dirs look like "card0-HDMI-A-1"; skip "card0", "renderD128", "version".
            if (n.compare(0, 4, "card") != 0) continue;
            if (n.find('-') == std::string::npos) continue;
            conns.push_back(n);
        }
        if (ec) { ok_ = false; err_ = "drm enumerate failed"; return; }
        std::sort(conns.begin(), conns.end());

        int connected_count = 0;
        for (const auto& n : conns) {
            fs::path base = fs::path("/sys/class/drm") / n;

            std::string status, enabled;
            { std::ifstream f(base / "status");  std::getline(f, status); }
            { std::ifstream f(base / "enabled"); std::getline(f, enabled); }

            // Connector label: strip leading "cardN-" prefix.
            std::string label = n;
            auto dash = label.find('-');
            if (dash != std::string::npos) label = label.substr(dash + 1);

            MonitorInfo m{};
            std::snprintf(m.name, sizeof(m.name), "%s", label.c_str());
            m.status   = status;
            m.enabled  = (enabled == "enabled");
            m.primary  = false;
            m.dragging = false;
            m.res_index = 0;
            m.rate_index = 0;

            // Real resolutions from /modes, deduped, preserving first-seen order.
            {
                std::ifstream f(base / "modes");
                std::string line;
                std::unordered_set<std::string> seen;
                while (std::getline(f, line)) {
                    while (!line.empty() && (line.back()=='\r'||line.back()==' '||line.back()=='\n'))
                        line.pop_back();
                    if (line.empty()) continue;
                    if (seen.insert(line).second) m.res_list.push_back(line);
                }
            }

            bool is_connected = (status == "connected");
            if (is_connected) ++connected_count;

            next.push_back(std::move(m));
        }

        // Lay connectors out left to right in the normalized canvas.
        int total = (int)next.size();
        for (int i = 0; i < total; ++i) {
            auto& m = next[i];
            m.w = 0.30f; m.h = 0.45f;
            m.x = 0.03f + (float)i * 0.33f;
            if (m.x > 1.0f - m.w) m.x = 1.0f - m.w;
            m.y = 0.20f;
        }
        // Mark the first connected+enabled connector primary (real signal, not random).
        for (auto& m : next) {
            if (m.status == "connected" && m.enabled) { m.primary = true; break; }
        }

        monitors = std::move(next);
        // Rebuild Combo c-string views (pointers are stable within each element).
        for (auto& m : monitors) {
            m.res_cstr.clear();
            for (auto& r : m.res_list) m.res_cstr.push_back(r.c_str());
            if (m.res_index >= (int)m.res_cstr.size()) m.res_index = 0;
        }
        if (selected_monitor >= (int)monitors.size()) selected_monitor = 0;

        if (ok_ && connected_count == 0 && monitors.empty())
            { /* no connectors found; leave fields empty, never fabricate */ }
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
};

inline void render_display_panel(DisplayState& st) {
    st.maybe_refresh();
    if (st.monitors.empty() && st.last_refresh_ <= -1.0e8) st.init();

    if (!st.ok_) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.3f, 1.0f));
        ImGui::TextWrapped("display source unavailable: %s", st.err_.c_str());
        ImGui::PopStyleColor();
        if (ImGui::SmallButton("Retry")) st.refresh();
        ImGui::Separator();
    }

    const ImVec4 accent(0.0f, 1.0f, 0.67f, 1.0f);

    ImGui::PushStyleColor(ImGuiCol_Text, accent);
    ImGui::Text("DISPLAY SETTINGS");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();

    // Profile bar
    ImGui::Text("Profile:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200);
    ImGui::Combo("##profile", &st.profile_index, DisplayState::profiles, DisplayState::num_profiles);
    ImGui::SameLine();
    if (ImGui::Button("Save Profile")) {
        st.show_save_dialog = true;
        memset(st.profile_name, 0, sizeof(st.profile_name));
    }
    ImGui::SameLine();
    if (ImGui::Button("Load Profile")) {
        // Simulate load
    }
    ImGui::Spacing();

    // Monitor layout area
    float layout_h = 260.0f;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 layout_size(avail.x, layout_h);

    ImGui::BeginChild("##layout", ImVec2(avail.x, layout_h), true);
    ImGui::TextDisabled("Monitor Layout (drag to reposition)");

    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float canvas_w = avail.x - 24.0f;
    float canvas_h = layout_h - 40.0f;

    // Draw grid
    for (int i = 0; i <= 10; ++i) {
        float x = origin.x + (canvas_w * i / 10.0f);
        float y = origin.y + (canvas_h * i / 10.0f);
        draw->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + canvas_h),
                      IM_COL32(40, 40, 60, 100));
        if (i <= 10)
            draw->AddLine(ImVec2(origin.x, y), ImVec2(origin.x + canvas_w, y),
                          IM_COL32(40, 40, 60, 100));
    }

    ImGuiIO& io = ImGui::GetIO();
    for (int i = 0; i < (int)st.monitors.size(); ++i) {
        auto& m = st.monitors[i];
        ImVec2 tl(origin.x + m.x * canvas_w, origin.y + m.y * canvas_h);
        ImVec2 br(tl.x + m.w * canvas_w, tl.y + m.h * canvas_h);

        bool hovered = io.MousePos.x >= tl.x && io.MousePos.x <= br.x &&
                       io.MousePos.y >= tl.y && io.MousePos.y <= br.y;

        ImU32 fill = (i == st.selected_monitor)
            ? IM_COL32(0, 100, 55, 180) : IM_COL32(30, 30, 50, 200);
        ImU32 border = hovered ? IM_COL32(0, 255, 136, 255) : IM_COL32(80, 80, 120, 255);

        draw->AddRectFilled(tl, br, fill, 4.0f);
        draw->AddRect(tl, br, border, 4.0f, 0, 2.0f);

        // Label
        const char* cur_res = (!m.res_cstr.empty() && m.res_index < (int)m.res_cstr.size())
                              ? m.res_cstr[m.res_index] : "no signal";
        char label[128];
        snprintf(label, sizeof(label), "%s\n%s", m.name, cur_res);
        draw->AddText(ImVec2(tl.x + 6, tl.y + 4), IM_COL32(220, 220, 220, 255), m.name);
        draw->AddText(ImVec2(tl.x + 6, tl.y + 22),
                      IM_COL32(150, 150, 150, 255),
                      cur_res);

        if (m.primary) {
            draw->AddText(ImVec2(tl.x + 6, br.y - 18), IM_COL32(0, 255, 136, 255), "[PRIMARY]");
        }

        // Click to select
        if (hovered && ImGui::IsMouseClicked(0)) {
            st.selected_monitor = i;
            m.dragging = true;
        }
        if (m.dragging) {
            if (ImGui::IsMouseDown(0)) {
                m.x += io.MouseDelta.x / canvas_w;
                m.y += io.MouseDelta.y / canvas_h;
                m.x = std::clamp(m.x, 0.0f, 1.0f - m.w);
                m.y = std::clamp(m.y, 0.0f, 1.0f - m.h);
            } else {
                m.dragging = false;
            }
        }
    }
    ImGui::EndChild();
    ImGui::Spacing();

    // Per-monitor settings
    if (st.selected_monitor >= 0 && st.selected_monitor < (int)st.monitors.size()) {
        auto& m = st.monitors[st.selected_monitor];

        ImGui::BeginChild("##monitor_settings", ImVec2(avail.x * 0.55f, 0), true);
        ImGui::TextColored(accent, "Monitor: %s", m.name);
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("Resolution:");
        ImGui::SameLine(120);
        ImGui::SetNextItemWidth(200);
        if (!m.res_cstr.empty()) {
            ImGui::Combo("##res", &m.res_index, m.res_cstr.data(), (int)m.res_cstr.size());
        } else {
            ImGui::TextDisabled("(no modes reported)");
        }

        ImGui::Text("Refresh Rate:");
        ImGui::SameLine(120);
        ImGui::SetNextItemWidth(200);
        // /sys/class/drm/<c>/modes does not expose refresh rate; no real source.
        ImGui::TextDisabled("(not reported by sysfs)");

        ImGui::Spacing();
        bool was_primary = m.primary;
        ImGui::Checkbox("Set as Primary", &m.primary);
        if (m.primary && !was_primary) {
            for (int j = 0; j < (int)st.monitors.size(); ++j) {
                if (j != st.selected_monitor) st.monitors[j].primary = false;
            }
        }

        ImGui::Spacing();
        if (ImGui::Button("Apply Changes", ImVec2(160, 32))) {
            // Apply monitor configuration
        }
        ImGui::EndChild();

        ImGui::SameLine();

        // Color / Night Mode
        ImGui::BeginChild("##color_settings", ImVec2(0, 0), true);
        ImGui::TextColored(accent, "Color & Night Mode");
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Checkbox("Night Mode", &st.night_mode);
        ImGui::Spacing();

        ImGui::Text("Color Temperature:");
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##temp", &st.color_temp, 2700.0f, 6500.0f, "%.0f K");

        // Visual indicator
        float t = (st.color_temp - 2700.0f) / (6500.0f - 2700.0f);
        ImVec4 warm(1.0f, 0.6f, 0.2f, 1.0f);
        ImVec4 cool(0.8f, 0.9f, 1.0f, 1.0f);
        ImVec4 preview(warm.x + t * (cool.x - warm.x),
                       warm.y + t * (cool.y - warm.y),
                       warm.z + t * (cool.z - warm.z), 1.0f);

        ImGui::Spacing();
        ImVec2 bar_pos = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(
            bar_pos, ImVec2(bar_pos.x + ImGui::GetContentRegionAvail().x, bar_pos.y + 24),
            ImGui::ColorConvertFloat4ToU32(preview), 3.0f);
        ImGui::Dummy(ImVec2(0, 30));

        ImGui::Text(st.night_mode ? "Night mode: Active" : "Night mode: Off");
        if (st.night_mode) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "Blue light reduced");
        }

        ImGui::EndChild();
    }

    // Save Profile Dialog
    if (st.show_save_dialog) {
        ImGui::OpenPopup("Save Profile");
        st.show_save_dialog = false;
    }
    if (ImGui::BeginPopupModal("Save Profile", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Profile Name:");
        ImGui::SetNextItemWidth(300);
        ImGui::InputText("##prof_name", st.profile_name, sizeof(st.profile_name));
        ImGui::Spacing();
        if (ImGui::Button("Save", ImVec2(120, 30))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 30))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

} // namespace straylight::display
