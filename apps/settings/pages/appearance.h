// apps/settings/pages/appearance.h
// Appearance settings — themes, fonts, accent color
#pragma once

#include "../settings_page.h"

#include <straylight/result.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace straylight::settings {

struct ThemeColors {
    uint32_t bg_primary = 0xFF1A1A2E;
    uint32_t bg_secondary = 0xFF16213E;
    uint32_t bg_tertiary = 0xFF0F3460;
    uint32_t fg_primary = 0xFFCCCCCC;
    uint32_t fg_secondary = 0xFF888888;
    uint32_t accent = 0xFF00FFAA;
    uint32_t error = 0xFFE74C3C;
    uint32_t warning = 0xFFF39C12;
    uint32_t success = 0xFF2ECC71;
};

struct Theme {
    std::string name;
    std::string description;
    ThemeColors colors;
};

struct AppearanceSettings {
    std::string theme_name = "cyberpunk";
    std::string font_family = "Inter";
    float font_size = 14.0f;
    float title_font_size = 18.0f;
    uint32_t accent_color = 0xFF00FFAA;
    bool animations_enabled = true;
    float window_opacity = 0.95f;
    float border_radius = 4.0f;
};

/// Appearance settings page.
class AppearancePage : public SettingsPage {
public:
    AppearancePage();

    [[nodiscard]] const char* label() const override { return "Appearance"; }

    /// Load appearance settings.
    void load() override;

    /// Apply appearance settings.
    Result<void, std::string> apply();

    /// Save settings.
    Result<void, std::string> save();

    /// Render the appearance settings page in ImGui.
    void render() override;

private:
    AppearanceSettings settings_;
    std::vector<Theme> themes_;
    bool dirty_ = false;

    void init_themes();
    void apply_theme(const Theme& theme);
};

} // namespace straylight::settings
