// apps/wizard/pages/tour.h
// "Would you like a guided tour?" — final wizard page
#pragma once

namespace straylight::wizard {

/// Result from the tour page.
enum class TourChoice {
    kPending,   ///< user hasn't decided yet
    kYes,       ///< launch the interactive tour
    kNo,        ///< skip — go straight to the desktop
};

/// Tour offer page — simple full-screen prompt with Yes / No buttons.
class TourPage {
public:
    TourPage() = default;
    ~TourPage() = default;

    /// Render the page.  Returns true (advance) when the user makes a choice.
    bool render();

    /// The choice the user made (valid after render() returns true).
    [[nodiscard]] TourChoice choice() const { return choice_; }

private:
    TourChoice choice_ = TourChoice::kPending;
};

} // namespace straylight::wizard
