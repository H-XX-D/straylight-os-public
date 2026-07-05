// apps/settings/pages/input.cpp
// Input settings — keyboard layout, repeat rate, mouse, touchpad configuration
#include "input.h"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace straylight::settings {

using json = nlohmann::json;
namespace fs = std::filesystem;

InputPage::InputPage() {
    load_keyboard_layouts();
}

void InputPage::load_keyboard_layouts() {
    keyboard_layouts_ = {
        {"us", "English (US)"},
        {"gb", "English (UK)"},
        {"de", "German"},
        {"fr", "French"},
        {"es", "Spanish"},
        {"it", "Italian"},
        {"pt", "Portuguese"},
        {"ru", "Russian"},
        {"jp", "Japanese"},
        {"kr", "Korean"},
        {"cn", "Chinese"},
        {"ar", "Arabic"},
        {"br", "Portuguese (Brazil)"},
        {"ca", "Canadian"},
        {"ch", "Swiss"},
        {"cz", "Czech"},
        {"dk", "Danish"},
        {"fi", "Finnish"},
        {"hu", "Hungarian"},
        {"no", "Norwegian"},
        {"pl", "Polish"},
        {"se", "Swedish"},
        {"tr", "Turkish"},
        {"ua", "Ukrainian"},
        {"latam", "Latin American"},
        {"dvorak", "Dvorak"},
        {"colemak", "Colemak"},
    };

    // Try to read system keyboard list
    std::ifstream layouts("/usr/share/X11/xkb/rules/base.lst");
    if (layouts.is_open()) {
        std::string line;
        bool in_layouts = false;
        while (std::getline(layouts, line)) {
            if (line.find("! layout") != std::string::npos) {
                in_layouts = true;
                continue;
            }
            if (in_layouts && !line.empty() && line[0] == '!') {
                break;
            }
            if (in_layouts && line.size() > 2 && line[0] == ' ') {
                // Parse "  us   English (US)"
                size_t code_start = line.find_first_not_of(' ');
                size_t code_end = line.find_first_of(' ', code_start);
                size_t name_start = line.find_first_not_of(' ', code_end);
                if (code_start != std::string::npos &&
                    code_end != std::string::npos &&
                    name_start != std::string::npos) {
                    std::string code = line.substr(code_start, code_end - code_start);
                    std::string name = line.substr(name_start);
                    // Check if already in list
                    bool found = false;
                    for (const auto& [c, n] : keyboard_layouts_) {
                        if (c == code) { found = true; break; }
                    }
                    if (!found) {
                        keyboard_layouts_.emplace_back(code, name);
                    }
                }
            }
        }
    }
}

void InputPage::load() {
    // Try loading from config file
    std::vector<std::string> paths;
    const char* xdg = getenv("XDG_CONFIG_HOME");
    const char* home = getenv("HOME");

    if (xdg) paths.push_back(std::string(xdg) + "/straylight/input.json");
    if (home) paths.push_back(std::string(home) + "/.config/straylight/input.json");

    for (const auto& path : paths) {
        std::ifstream file(path);
        if (!file.is_open()) continue;

        json j;
        try { file >> j; } catch (...) { continue; }

        if (j.contains("keyboard_layout"))
            settings_.keyboard_layout = j["keyboard_layout"].get<std::string>();
        if (j.contains("keyboard_variant"))
            settings_.keyboard_variant = j["keyboard_variant"].get<std::string>();
        if (j.contains("repeat_delay_ms"))
            settings_.repeat_delay_ms = j["repeat_delay_ms"].get<int>();
        if (j.contains("repeat_rate_hz"))
            settings_.repeat_rate_hz = j["repeat_rate_hz"].get<int>();
        if (j.contains("mouse_sensitivity"))
            settings_.mouse_sensitivity = j["mouse_sensitivity"].get<float>();
        if (j.contains("mouse_natural_scroll"))
            settings_.mouse_natural_scroll = j["mouse_natural_scroll"].get<bool>();
        if (j.contains("mouse_accel_speed"))
            settings_.mouse_accel_speed = j["mouse_accel_speed"].get<float>();
        if (j.contains("mouse_accel_profile"))
            settings_.mouse_accel_profile = j["mouse_accel_profile"].get<std::string>();
        if (j.contains("touchpad_tap_to_click"))
            settings_.touchpad_tap_to_click = j["touchpad_tap_to_click"].get<bool>();
        if (j.contains("touchpad_natural_scroll"))
            settings_.touchpad_natural_scroll = j["touchpad_natural_scroll"].get<bool>();
        if (j.contains("touchpad_sensitivity"))
            settings_.touchpad_sensitivity = j["touchpad_sensitivity"].get<float>();
        if (j.contains("touchpad_disable_while_typing"))
            settings_.touchpad_disable_while_typing = j["touchpad_disable_while_typing"].get<bool>();

        break;
    }
}

Result<void, std::string> InputPage::apply() {
    // Apply keyboard layout via setxkbmap equivalent
    // In Wayland, this is done through the compositor
    json config;
    config["keyboard_layout"] = settings_.keyboard_layout;
    config["keyboard_variant"] = settings_.keyboard_variant;
    config["keyboard_model"] = settings_.keyboard_model;
    config["repeat_delay_ms"] = settings_.repeat_delay_ms;
    config["repeat_rate_hz"] = settings_.repeat_rate_hz;
    config["mouse_sensitivity"] = settings_.mouse_sensitivity;
    config["mouse_natural_scroll"] = settings_.mouse_natural_scroll;
    config["mouse_accel_speed"] = settings_.mouse_accel_speed;
    config["mouse_accel_profile"] = settings_.mouse_accel_profile;
    config["touchpad_tap_to_click"] = settings_.touchpad_tap_to_click;
    config["touchpad_natural_scroll"] = settings_.touchpad_natural_scroll;
    config["touchpad_sensitivity"] = settings_.touchpad_sensitivity;
    config["touchpad_disable_while_typing"] = settings_.touchpad_disable_while_typing;
    config["touchpad_click_method"] = settings_.touchpad_click_method;

    // Write to a runtime config that the distro desktop integration can consume.
    std::ofstream file("/tmp/straylight-input-config.json");
    if (!file.is_open()) {
        return Result<void, std::string>::error("Cannot write input config");
    }
    file << config.dump(2);
    file.close();

    dirty_ = false;
    return Result<void, std::string>::ok();
}

Result<void, std::string> InputPage::save() {
    const char* home = getenv("HOME");
    if (!home) return Result<void, std::string>::error("HOME not set");

    std::string dir = std::string(home) + "/.config/straylight";
    std::error_code ec;
    fs::create_directories(dir, ec);

    std::string path = dir + "/input.json";
    json j;
    j["keyboard_layout"] = settings_.keyboard_layout;
    j["keyboard_variant"] = settings_.keyboard_variant;
    j["keyboard_model"] = settings_.keyboard_model;
    j["repeat_delay_ms"] = settings_.repeat_delay_ms;
    j["repeat_rate_hz"] = settings_.repeat_rate_hz;
    j["mouse_sensitivity"] = settings_.mouse_sensitivity;
    j["mouse_natural_scroll"] = settings_.mouse_natural_scroll;
    j["mouse_accel_speed"] = settings_.mouse_accel_speed;
    j["mouse_accel_profile"] = settings_.mouse_accel_profile;
    j["touchpad_tap_to_click"] = settings_.touchpad_tap_to_click;
    j["touchpad_natural_scroll"] = settings_.touchpad_natural_scroll;
    j["touchpad_sensitivity"] = settings_.touchpad_sensitivity;
    j["touchpad_disable_while_typing"] = settings_.touchpad_disable_while_typing;
    j["touchpad_click_method"] = settings_.touchpad_click_method;

    std::ofstream file(path);
    if (!file.is_open()) {
        return Result<void, std::string>::error("Cannot save input config");
    }
    file << j.dump(2);

    return Result<void, std::string>::ok();
}

void InputPage::render() {
    ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.67f, 1.0f), "Input Settings");
    ImGui::Separator();

    // === Keyboard ===
    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Keyboard");

    // Layout combo
    ImGui::Text("Layout:");
    std::string current_label;
    for (const auto& [code, name] : keyboard_layouts_) {
        if (code == settings_.keyboard_layout) {
            current_label = code + " - " + name;
            break;
        }
    }
    if (current_label.empty()) current_label = settings_.keyboard_layout;

    if (ImGui::BeginCombo("##KBLayout", current_label.c_str())) {
        for (const auto& [code, name] : keyboard_layouts_) {
            std::string label = code + " - " + name;
            bool selected = (code == settings_.keyboard_layout);
            if (ImGui::Selectable(label.c_str(), selected)) {
                settings_.keyboard_layout = code;
                dirty_ = true;
            }
        }
        ImGui::EndCombo();
    }

    // Repeat delay
    ImGui::Text("Key Repeat Delay (ms):");
    if (ImGui::SliderInt("##RepeatDelay", &settings_.repeat_delay_ms, 100, 1000)) {
        dirty_ = true;
    }

    // Repeat rate
    ImGui::Text("Key Repeat Rate (Hz):");
    if (ImGui::SliderInt("##RepeatRate", &settings_.repeat_rate_hz, 1, 100)) {
        dirty_ = true;
    }

    ImGui::Spacing();
    ImGui::Separator();

    // === Mouse ===
    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Mouse");

    ImGui::Text("Sensitivity:");
    if (ImGui::SliderFloat("##MouseSens", &settings_.mouse_sensitivity,
                            0.1f, 5.0f, "%.2f")) {
        dirty_ = true;
    }

    if (ImGui::Checkbox("Natural Scroll##Mouse",
                         &settings_.mouse_natural_scroll)) {
        dirty_ = true;
    }

    ImGui::Text("Acceleration Speed:");
    if (ImGui::SliderFloat("##MouseAccel", &settings_.mouse_accel_speed,
                            -1.0f, 1.0f, "%.2f")) {
        dirty_ = true;
    }

    ImGui::Text("Acceleration Profile:");
    bool flat = (settings_.mouse_accel_profile == "flat");
    bool adaptive = (settings_.mouse_accel_profile == "adaptive");
    if (ImGui::RadioButton("Flat##MouseP", flat)) {
        settings_.mouse_accel_profile = "flat";
        dirty_ = true;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Adaptive##MouseP", adaptive)) {
        settings_.mouse_accel_profile = "adaptive";
        dirty_ = true;
    }

    ImGui::Spacing();
    ImGui::Separator();

    // === Touchpad ===
    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Touchpad");

    if (ImGui::Checkbox("Tap to Click", &settings_.touchpad_tap_to_click)) {
        dirty_ = true;
    }
    if (ImGui::Checkbox("Natural Scroll##Touchpad",
                         &settings_.touchpad_natural_scroll)) {
        dirty_ = true;
    }
    if (ImGui::Checkbox("Two Finger Scroll",
                         &settings_.touchpad_two_finger_scroll)) {
        dirty_ = true;
    }
    if (ImGui::Checkbox("Disable While Typing",
                         &settings_.touchpad_disable_while_typing)) {
        dirty_ = true;
    }

    ImGui::Text("Touchpad Sensitivity:");
    if (ImGui::SliderFloat("##TouchSens", &settings_.touchpad_sensitivity,
                            0.1f, 5.0f, "%.2f")) {
        dirty_ = true;
    }

    ImGui::Text("Click Method:");
    bool cf = (settings_.touchpad_click_method == "clickfinger");
    bool ba = (settings_.touchpad_click_method == "buttonareas");
    if (ImGui::RadioButton("Clickfinger", cf)) {
        settings_.touchpad_click_method = "clickfinger";
        dirty_ = true;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Button Areas", ba)) {
        settings_.touchpad_click_method = "buttonareas";
        dirty_ = true;
    }

    ImGui::Spacing();
    ImGui::Separator();

    if (dirty_) {
        if (ImGui::Button("Apply", ImVec2(120, 30))) {
            apply();
            save();
        }
    }
}

} // namespace straylight::settings
