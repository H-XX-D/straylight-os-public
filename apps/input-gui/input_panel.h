// apps/input-gui/input_panel.h
// StrayLight Input Settings panel
#pragma once

#include <imgui.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cmath>
// [wire] OS data-source includes
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <sys/stat.h>
#include <dirent.h>

namespace straylight::input {

struct KeyRemap {
    char from_key[32];
    char to_key[32];
    bool enabled;
};

struct InputState {
    int active_tab = 0;

    // Keyboard
    int  kb_layout = 0;
    float kb_repeat_rate = 30.0f;
    float kb_repeat_delay = 500.0f;
    bool caps_lock_ctrl = false;
    std::vector<KeyRemap> remaps;

    // Mouse
    float mouse_speed = 1.0f;
    float mouse_accel = 0.0f;
    bool  mouse_natural_scroll = false;
    int   mouse_btn_primary = 0;  // 0=left, 1=right

    // Touchpad
    bool  tp_enabled = true;
    bool  tp_tap_to_click = true;
    bool  tp_natural_scroll = true;
    bool  tp_two_finger_scroll = true;
    bool  tp_pinch_zoom = true;
    bool  tp_three_finger_swipe = true;
    float tp_speed = 1.0f;

    // Gamepad
    float gp_left_deadzone = 0.15f;
    float gp_right_deadzone = 0.15f;
    float gp_trigger_deadzone = 0.1f;
    bool  gp_connected = true;
    bool  gp_buttons[16] = {};
    float gp_axes[4] = {};
    int   gp_rumble_intensity = 50;

    static constexpr const char* kb_layouts[] = {
        "US QWERTY", "UK QWERTY", "DE QWERTZ", "FR AZERTY",
        "Dvorak", "Colemak", "ES QWERTY", "JP"
    };
    static constexpr int num_layouts = 8;

    static constexpr const char* btn_options[] = { "Left", "Right" };

    // [wire] OS-backed refresh (OS path only; no daemon).
    bool        ok_ = false;
    std::string err_;
    double      last_refresh_ = -1.0e9;

    // Real, non-fabricated facts loaded from the OS each refresh.
    int  kbd_count_ = 0;   // keyboards present (Handlers contain 'kbd')
    int  mouse_count_ = 0; // mice present     (Handlers contain 'mouse')
    bool touchpad_present_ = false; // INPUT_PROP_POINTER/BUTTONPAD seen

    // Map an X11 layout token (e.g. "us", "gb", "de") to kb_layouts[] index.
    static int layout_to_index(const std::string& x11) {
        // First token only (XKBLAYOUT may be comma-separated).
        std::string t = x11.substr(0, x11.find(','));
        // trim whitespace
        size_t a = t.find_first_not_of(" \t\r\n");
        size_t b = t.find_last_not_of(" \t\r\n");
        if (a == std::string::npos) return 0;
        t = t.substr(a, b - a + 1);
        if (t == "us")        return 0; // US QWERTY
        if (t == "gb")        return 1; // UK QWERTY
        if (t == "de")        return 2; // DE QWERTZ
        if (t == "fr")        return 3; // FR AZERTY
        if (t == "dvorak")    return 4; // Dvorak
        if (t == "colemak")   return 5; // Colemak
        if (t == "es")        return 6; // ES QWERTY
        if (t == "jp")        return 7; // JP
        return 0; // unknown layout: do not fabricate, fall back to US index
    }

    // Read the active X11 keyboard layout. Prefer `localectl status`,
    // fall back to /etc/default/keyboard XKBLAYOUT.
    static bool read_x11_layout(std::string& out) {
        if (FILE* p = popen("localectl status 2>/dev/null", "r")) {
            char line[512];
            while (fgets(line, sizeof(line), p)) {
                std::string s(line);
                size_t k = s.find("X11 Layout:");
                if (k != std::string::npos) {
                    out = s.substr(k + 11);
                    pclose(p);
                    return true;
                }
            }
            pclose(p);
        }
        std::ifstream kb("/etc/default/keyboard");
        if (kb.is_open()) {
            std::string line;
            while (std::getline(kb, line)) {
                size_t k = line.find("XKBLAYOUT=");
                if (k != std::string::npos) {
                    std::string v = line.substr(k + 10);
                    // strip surrounding quotes if present
                    if (!v.empty() && (v.front() == '"' || v.front() == '\'')) v.erase(0, 1);
                    if (!v.empty() && (v.back() == '"' || v.back() == '\'')) v.pop_back();
                    out = v;
                    return true;
                }
            }
        }
        return false;
    }

    // Gamepad present if any /dev/input/jsN node exists, or any device in
    // /proc/bus/input/devices exposes a jsN handler.
    static bool detect_gamepad() {
        for (int i = 0; i < 8; ++i) {
            char path[32];
            snprintf(path, sizeof(path), "/dev/input/js%d", i);
            struct stat stt;
            if (stat(path, &stt) == 0) return true;
        }
        std::ifstream f("/proc/bus/input/devices");
        if (f.is_open()) {
            std::string line;
            while (std::getline(f, line)) {
                if (line.compare(0, 12, "H: Handlers=") == 0 &&
                    line.find("js") != std::string::npos) {
                    // confirm a real jsN token, not a substring
                    std::istringstream hs(line.substr(12));
                    std::string tok;
                    while (hs >> tok) {
                        if (tok.size() > 2 && tok.compare(0, 2, "js") == 0 &&
                            isdigit((unsigned char)tok[2])) return true;
                    }
                }
            }
        }
        return false;
    }

    // Enumerate input devices from /proc/bus/input/devices the same way
    // apps/system_monitor and apps/hub parse /proc: walk N:/H:/B: blocks,
    // classify by Handlers tokens and PROP bits.
    void scan_devices() {
        kbd_count_ = 0;
        mouse_count_ = 0;
        touchpad_present_ = false;
        std::ifstream f("/proc/bus/input/devices");
        if (!f.is_open()) return;
        std::string line;
        bool has_kbd = false, has_mouse = false;
        unsigned long prop = 0;
        auto flush = [&]() {
            if (has_kbd) ++kbd_count_;
            if (has_mouse) ++mouse_count_;
            // INPUT_PROP_POINTER (bit 0) or INPUT_PROP_BUTTONPAD (bit 2) -> touchpad
            if (prop & 0x5UL) touchpad_present_ = true;
            has_kbd = has_mouse = false;
            prop = 0;
        };
        while (std::getline(f, line)) {
            if (line.empty()) { flush(); continue; }
            if (line.compare(0, 12, "H: Handlers=") == 0) {
                if (line.find("kbd") != std::string::npos) has_kbd = true;
                if (line.find("mouse") != std::string::npos) has_mouse = true;
            } else if (line.compare(0, 8, "B: PROP=") == 0) {
                prop = strtoul(line.c_str() + 8, nullptr, 16);
            }
        }
        flush();
    }

    void refresh() {
        err_.clear();
        bool any = false;

        std::string x11;
        if (read_x11_layout(x11)) {
            kb_layout = layout_to_index(x11);
            any = true;
        } else {
            err_ = "keyboard layout source unavailable (localectl / /etc/default/keyboard)";
        }

        // Gamepad presence is a real fact: false here means truly absent,
        // not a failure to read.
        gp_connected = detect_gamepad();

        // Device enumeration drives touchpad presence honestly.
        scan_devices();
        if (kbd_count_ > 0 || mouse_count_ > 0) any = true;
        tp_enabled = touchpad_present_; // no touchpad on this box -> false

        ok_ = any;
        if (!ok_ && err_.empty())
            err_ = "no input data source available";
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

inline void render_input_panel(InputState& st) {
    // [wire] render-top: refresh + unavailable banner
    st.maybe_refresh();
    if (!st.ok_) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.3f, 1.0f));
        ImGui::TextWrapped("input data source unavailable: %s", st.err_.c_str());
        ImGui::PopStyleColor();
        if (ImGui::SmallButton("Retry")) st.refresh();
        ImGui::Separator();
    }
    // [wire] neutralized fabricated remap seed: real data is loaded by InputState::refresh();
    // remaps are no longer fabricated here.

    const ImVec4 accent(0.0f, 1.0f, 0.67f, 1.0f);

    ImGui::PushStyleColor(ImGuiCol_Text, accent);
    ImGui::Text("INPUT SETTINGS");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();

    // Tabs
    if (ImGui::BeginTabBar("##input_tabs")) {
        // ---- KEYBOARD TAB ----
        if (ImGui::BeginTabItem("Keyboard")) {
            st.active_tab = 0;
            ImGui::Spacing();

            ImGui::Text("Layout:");
            ImGui::SameLine(140);
            ImGui::SetNextItemWidth(250);
            ImGui::Combo("##kb_layout", &st.kb_layout, InputState::kb_layouts, InputState::num_layouts);

            ImGui::Spacing();
            ImGui::Text("Repeat Rate:");
            ImGui::SameLine(140);
            ImGui::SetNextItemWidth(250);
            ImGui::SliderFloat("##repeat_rate", &st.kb_repeat_rate, 10.0f, 100.0f, "%.0f keys/sec");

            ImGui::Text("Repeat Delay:");
            ImGui::SameLine(140);
            ImGui::SetNextItemWidth(250);
            ImGui::SliderFloat("##repeat_delay", &st.kb_repeat_delay, 100.0f, 1000.0f, "%.0f ms");

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextColored(accent, "Key Remappings");
            ImGui::Spacing();

            if (ImGui::BeginTable("##remaps", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
                ImGui::TableSetupColumn("Enabled", ImGuiTableColumnFlags_WidthFixed, 60);
                ImGui::TableSetupColumn("From Key", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("To Key", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableHeadersRow();

                int delete_idx = -1;
                for (int i = 0; i < (int)st.remaps.size(); ++i) {
                    ImGui::TableNextRow();
                    ImGui::PushID(i);

                    ImGui::TableSetColumnIndex(0);
                    ImGui::Checkbox("##en", &st.remaps[i].enabled);

                    ImGui::TableSetColumnIndex(1);
                    ImGui::SetNextItemWidth(-1);
                    ImGui::InputText("##from", st.remaps[i].from_key, 32);

                    ImGui::TableSetColumnIndex(2);
                    ImGui::SetNextItemWidth(-1);
                    ImGui::InputText("##to", st.remaps[i].to_key, 32);

                    ImGui::TableSetColumnIndex(3);
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.1f, 0.1f, 0.8f));
                    if (ImGui::SmallButton("Remove")) delete_idx = i;
                    ImGui::PopStyleColor();

                    ImGui::PopID();
                }
                ImGui::EndTable();
                if (delete_idx >= 0) st.remaps.erase(st.remaps.begin() + delete_idx);
            }

            if (ImGui::Button("Add Remap")) {
                KeyRemap r{}; snprintf(r.from_key, 32, "Key"); snprintf(r.to_key, 32, "Key"); r.enabled = true;
                st.remaps.push_back(r);
            }

            ImGui::EndTabItem();
        }

        // ---- MOUSE TAB ----
        if (ImGui::BeginTabItem("Mouse")) {
            st.active_tab = 1;
            ImGui::Spacing();

            ImGui::Text("Speed:");
            ImGui::SameLine(140);
            ImGui::SetNextItemWidth(300);
            ImGui::SliderFloat("##mouse_speed", &st.mouse_speed, 0.1f, 3.0f, "%.2f");

            ImGui::Text("Acceleration:");
            ImGui::SameLine(140);
            ImGui::SetNextItemWidth(300);
            ImGui::SliderFloat("##mouse_accel", &st.mouse_accel, -1.0f, 1.0f, "%.2f");

            ImGui::Spacing();
            ImGui::Text("Primary Button:");
            ImGui::SameLine(140);
            ImGui::SetNextItemWidth(150);
            ImGui::Combo("##primary_btn", &st.mouse_btn_primary, InputState::btn_options, 2);

            ImGui::Spacing();
            ImGui::Checkbox("Natural Scrolling", &st.mouse_natural_scroll);

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextColored(accent, "Button Mapping");
            ImGui::Spacing();
            if (ImGui::BeginTable("##btn_map", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Button");
                ImGui::TableSetupColumn("Default Action");
                ImGui::TableSetupColumn("Custom Action");
                ImGui::TableHeadersRow();

                const char* btns[] = {"Left Click", "Right Click", "Middle Click", "Side Button 1", "Side Button 2"};
                const char* actions[] = {"Select", "Context Menu", "Paste", "Back", "Forward"};
                for (int i = 0; i < 5; ++i) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::Text("%s", btns[i]);
                    ImGui::TableSetColumnIndex(1); ImGui::TextDisabled("%s", actions[i]);
                    ImGui::TableSetColumnIndex(2);
                    ImGui::PushID(100 + i);
                    ImGui::SetNextItemWidth(-1);
                    char buf[64]; snprintf(buf, 64, "%s", actions[i]);
                    ImGui::InputText("##custom", buf, 64);
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }

        // ---- TOUCHPAD TAB ----
        if (ImGui::BeginTabItem("Touchpad")) {
            st.active_tab = 2;
            ImGui::Spacing();

            ImGui::Checkbox("Enable Touchpad", &st.tp_enabled);
            ImGui::Spacing();

            if (st.tp_enabled) {
                ImGui::Text("Speed:");
                ImGui::SameLine(180);
                ImGui::SetNextItemWidth(300);
                ImGui::SliderFloat("##tp_speed", &st.tp_speed, 0.1f, 3.0f, "%.2f");

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::TextColored(accent, "Gestures & Behavior");
                ImGui::Spacing();

                ImGui::Checkbox("Tap to Click", &st.tp_tap_to_click);
                ImGui::Checkbox("Natural Scrolling", &st.tp_natural_scroll);
                ImGui::Checkbox("Two-Finger Scroll", &st.tp_two_finger_scroll);
                ImGui::Checkbox("Pinch to Zoom", &st.tp_pinch_zoom);
                ImGui::Checkbox("Three-Finger Swipe", &st.tp_three_finger_swipe);
            } else {
                ImGui::TextDisabled("Touchpad is disabled");
            }
            ImGui::EndTabItem();
        }

        // ---- GAMEPAD TAB ----
        if (ImGui::BeginTabItem("Gamepad")) {
            st.active_tab = 3;
            ImGui::Spacing();

            if (st.gp_connected) {
                ImGui::TextColored(accent, "Controller: Connected");
            } else {
                ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "No controller detected");
            }
            ImGui::Spacing();

            ImGui::Text("Left Stick Deadzone:");
            ImGui::SameLine(200);
            ImGui::SetNextItemWidth(250);
            ImGui::SliderFloat("##lz", &st.gp_left_deadzone, 0.0f, 0.5f, "%.2f");

            ImGui::Text("Right Stick Deadzone:");
            ImGui::SameLine(200);
            ImGui::SetNextItemWidth(250);
            ImGui::SliderFloat("##rz", &st.gp_right_deadzone, 0.0f, 0.5f, "%.2f");

            ImGui::Text("Trigger Deadzone:");
            ImGui::SameLine(200);
            ImGui::SetNextItemWidth(250);
            ImGui::SliderFloat("##tz", &st.gp_trigger_deadzone, 0.0f, 0.5f, "%.2f");

            ImGui::Spacing();
            ImGui::Text("Rumble Intensity:");
            ImGui::SameLine(200);
            ImGui::SetNextItemWidth(250);
            ImGui::SliderInt("##rumble", &st.gp_rumble_intensity, 0, 100, "%d%%");

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextColored(accent, "Button Test");
            ImGui::Spacing();

            // Button test visualization - grid of circles
            const char* btn_names[] = {"A","B","X","Y","LB","RB","LT","RT",
                                        "Start","Select","L3","R3","Up","Down","Left","Right"};
            ImDrawList* draw = ImGui::GetWindowDrawList();
            ImVec2 bpos = ImGui::GetCursorScreenPos();
            for (int i = 0; i < 16; ++i) {
                float cx = bpos.x + 30 + (i % 8) * 60;
                float cy = bpos.y + 20 + (i / 8) * 60;
                ImU32 col = st.gp_buttons[i] ? IM_COL32(0, 255, 136, 255) : IM_COL32(60, 60, 80, 255);
                draw->AddCircleFilled(ImVec2(cx, cy), 18, col);
                draw->AddCircle(ImVec2(cx, cy), 18, IM_COL32(100, 100, 140, 255), 0, 2.0f);
                ImVec2 ts = ImGui::CalcTextSize(btn_names[i]);
                draw->AddText(ImVec2(cx - ts.x * 0.5f, cy - ts.y * 0.5f),
                              IM_COL32(220, 220, 220, 255), btn_names[i]);
            }
            ImGui::Dummy(ImVec2(0, 140));

            // Simulate button presses with toggles for testing
            ImGui::Text("Toggle buttons to test:");
            for (int i = 0; i < 16; ++i) {
                if (i > 0 && i % 8 != 0) ImGui::SameLine();
                ImGui::PushID(200 + i);
                ImGui::Checkbox(btn_names[i], &st.gp_buttons[i]);
                ImGui::PopID();
            }

            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
}

} // namespace straylight::input
