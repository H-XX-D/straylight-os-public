// apps/oobe/pages/package_profile.h
// OOBE package profile selection page
#pragma once

#include <string>
#include <vector>

namespace straylight::oobe {

/// A package profile definition.
struct PackageProfile {
    std::string name;
    std::string description;
    std::vector<std::string> install_packages;
    std::vector<std::string> remove_packages;
};

/// Package profile selection page — choose ML Workstation, Developer,
/// Server, or Minimal profile.
class PackageProfilePage {
public:
    PackageProfilePage();
    ~PackageProfilePage() = default;

    /// Render the page. Returns true to advance.
    bool render();

    /// Get the built-in profiles (for testing).
    static std::vector<PackageProfile> builtin_profiles();

    /// Get the selected profile index (-1 for skip).
    [[nodiscard]] int selected_index() const { return selected_; }

private:
    std::vector<PackageProfile> profiles_;
    int selected_ = -1;
    float progress_ = 0.0f;
    bool installing_ = false;
};

} // namespace straylight::oobe
