// apps/oobe/pages/welcome.h
// OOBE welcome page
#pragma once

namespace straylight::oobe {

/// First page of the OOBE wizard — welcome screen with branding.
/// Displays the StrayLight logo, headline, and "Get Started" button.
class WelcomePage {
public:
    WelcomePage() = default;
    ~WelcomePage() = default;

    /// Render the welcome page.
    /// @return true if the user clicked "Get Started" (advance to next page).
    bool render();
};

} // namespace straylight::oobe
