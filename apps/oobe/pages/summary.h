// apps/oobe/pages/summary.h
// OOBE summary page — review and apply
#pragma once

#include <string>

namespace straylight::oobe {

/// Summary page — reviews all OOBE selections and applies changes.
class SummaryPage {
public:
    SummaryPage() = default;
    ~SummaryPage() = default;

    /// Set the summary text for each section.
    void set_user_info(const std::string& info);
    void set_profile_info(const std::string& info);
    void set_network_info(const std::string& info);

    /// Render the summary page.
    /// @return  1 = "Apply and Continue", -1 = "Back", 0 = no action
    int render();

private:
    std::string user_info_    = "Default user";
    std::string profile_info_ = "No profile selected";
    std::string network_info_ = "No network configured";
};

} // namespace straylight::oobe
