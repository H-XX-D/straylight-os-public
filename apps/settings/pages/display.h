// apps/settings/pages/display.h
// Display settings — resolution, refresh rate, scaling
#pragma once

#include "../settings_page.h"

#include <straylight/result.h>

#include <string>
#include <vector>

namespace straylight::settings {

struct DisplayMode {
    int width = 0;
    int height = 0;
    float refresh_rate = 0.0f;
    bool current = false;
};

struct DisplayOutput {
    std::string name;
    std::string description;
    std::string make;
    std::string model;
    int physical_width_mm = 0;
    int physical_height_mm = 0;
    bool enabled = true;
    int x = 0;
    int y = 0;
    float scale = 1.0f;
    int transform = 0; // 0=normal, 1=90, 2=180, 3=270

    std::vector<DisplayMode> modes;
    int current_mode_index = 0;
};

/// Display settings page — detects outputs from DRM sysfs and applies
/// configuration via wlr-output-management or compositor IPC.
class DisplayPage : public SettingsPage {
public:
    DisplayPage();

    [[nodiscard]] const char* label() const override { return "Display"; }

    /// Detect connected displays and populate outputs_.
    void load() override;

    /// Render the display settings page in ImGui.
    void render() override;

    /// Apply display configuration via wlr-output-management protocol.
    Result<void, std::string> apply();

    /// Get the outputs.
    [[nodiscard]] const std::vector<DisplayOutput>& outputs() const {
        return outputs_;
    }

private:
    void detect();
    void read_drm_outputs();

    std::vector<DisplayOutput> outputs_;
    int selected_output_ = 0;
    bool dirty_ = false;
};

} // namespace straylight::settings
