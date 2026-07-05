// apps/settings/settings_page.h
// Abstract base for all settings pages.
#pragma once

namespace straylight::settings {

/// Abstract interface for a single settings page.
/// Each page has a sidebar label, a load() method that reads current system
/// state, and a render() method that draws the ImGui panel content.
class SettingsPage {
public:
    virtual ~SettingsPage() = default;

    /// Short label displayed in the sidebar navigation list.
    [[nodiscard]] virtual const char* label() const = 0;

    /// Read current system state into the page's member variables.
    /// Called once on startup and again after any apply operation.
    virtual void load() = 0;

    /// Draw the page content using ImGui.
    /// Called every frame when this page is selected.
    virtual void render() = 0;
};

} // namespace straylight::settings
