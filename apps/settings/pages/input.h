// apps/settings/pages/input.h
// Input settings — keyboard layout, repeat rate, mouse, touchpad
#pragma once

#include "../settings_page.h"

#include <straylight/result.h>

#include <string>
#include <vector>

namespace straylight::settings {

struct InputSettings {
    // Keyboard
    std::string keyboard_layout = "us";
    std::string keyboard_variant;
    std::string keyboard_model = "pc105";
    int repeat_delay_ms = 300;
    int repeat_rate_hz = 25;

    // Mouse
    float mouse_sensitivity = 1.0f;  // 0.1..5.0
    bool mouse_natural_scroll = false;
    float mouse_accel_speed = 0.0f;  // -1.0..1.0
    std::string mouse_accel_profile = "adaptive"; // flat, adaptive

    // Touchpad
    bool touchpad_tap_to_click = true;
    bool touchpad_natural_scroll = true;
    bool touchpad_two_finger_scroll = true;
    float touchpad_sensitivity = 1.0f;
    bool touchpad_disable_while_typing = true;
    std::string touchpad_click_method = "clickfinger"; // buttonareas, clickfinger
};

/// Input settings page.
class InputPage : public SettingsPage {
public:
    InputPage();

    [[nodiscard]] const char* label() const override { return "Input"; }

    /// Load current input settings.
    void load() override;

    /// Apply input settings.
    Result<void, std::string> apply();

    /// Save settings to config file.
    Result<void, std::string> save();

    /// Render the input settings page in ImGui.
    void render() override;

    [[nodiscard]] const InputSettings& settings() const { return settings_; }

private:
    InputSettings settings_;
    bool dirty_ = false;

    // Available keyboard layouts
    std::vector<std::pair<std::string, std::string>> keyboard_layouts_;
    void load_keyboard_layouts();
};

} // namespace straylight::settings
