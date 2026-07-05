// apps/wizard/pages/summary.h
// Wizard summary page
#pragma once

#include <string>

namespace straylight::wizard {

/// Summary page — shows final configuration and "Finish" button.
class SummaryPage {
public:
    SummaryPage() = default;
    ~SummaryPage() = default;

    void set_theme(const std::string& theme);
    void set_layout(const std::string& layout);
    void set_gpu_profile(const std::string& profile);

    /// Render the page. Returns true when user clicks "Finish".
    bool render();

private:
    std::string theme_       = "default";
    std::string layout_      = "top_bar + left_dock";
    std::string gpu_profile_ = "balanced";
};

} // namespace straylight::wizard
