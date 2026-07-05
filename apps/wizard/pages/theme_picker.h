// apps/wizard/pages/theme_picker.h
// Theme picker page for the post-login wizard
#pragma once

#include <string>

namespace straylight::wizard {

/// Theme picker page — choose between Default, Cyberpunk, and Minimal
/// themes with live ImGui preview.
class ThemePickerPage {
public:
    ThemePickerPage() = default;
    ~ThemePickerPage() = default;

    /// Render the page. Returns true to advance.
    bool render();

    /// Get the selected theme name.
    [[nodiscard]] const std::string& selected_theme() const {
        return selected_;
    }

private:
    std::string selected_ = "default";
    int selected_index_ = 0;
};

} // namespace straylight::wizard
