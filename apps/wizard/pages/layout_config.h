// apps/wizard/pages/layout_config.h
// Layout configuration page for the wizard
#pragma once

namespace straylight::wizard {

/// Layout configuration page — toggle top bar, left dock, bottom dock.
class LayoutConfigPage {
public:
    LayoutConfigPage() = default;
    ~LayoutConfigPage() = default;

    /// Render the page. Returns true to advance.
    bool render();

    /// Get current toggle states (for testing).
    [[nodiscard]] bool top_bar_enabled() const { return top_bar_; }
    [[nodiscard]] bool left_dock_enabled() const { return left_dock_; }
    [[nodiscard]] bool bottom_dock_enabled() const { return bottom_dock_; }

private:
    bool top_bar_     = true;   // always on, shown as disabled checkbox
    bool left_dock_   = true;   // enabled by default
    bool bottom_dock_ = false;  // disabled by default
};

} // namespace straylight::wizard
