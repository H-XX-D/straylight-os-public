// apps/wizard/pages/ide_setup.h
// IDE selection and automatic installation for the post-login wizard
#pragma once

#include <string>
#include <vector>

namespace straylight::wizard {

/// A single IDE entry shown in the picker.
struct IdeEntry {
    std::string id;           // internal key: "vscode", "neovim", …
    std::string display_name; // "Visual Studio Code"
    std::string description;  // short one-liner
    std::string install_cmd;  // shell command that installs it (flatpak/apt)
    bool        selected   = false;
    bool        installed  = false;  // detected as already present
};

/// Installation state for a single IDE.
enum class IdeInstallStatus {
    kIdle,
    kDownloading,
    kInstalling,
    kDone,
    kFailed,
};

/// IDE setup page — choose one or more IDEs, install them via flatpak/apt.
class IdeSetupPage {
public:
    IdeSetupPage();
    ~IdeSetupPage() = default;

    /// Render the page.  Returns true when the user clicks "Next".
    bool render();

    /// IDs of the IDEs the user selected (may be empty if skipped).
    [[nodiscard]] const std::vector<std::string>& selected_ids() const {
        return selected_ids_;
    }

private:
    std::vector<IdeEntry>     ides_;
    std::vector<std::string>  selected_ids_;
    IdeInstallStatus          install_status_ = IdeInstallStatus::kIdle;
    std::string               install_log_;
    std::string               current_installing_;
    bool                      detect_done_ = false;

    void detect_installed();
    void install_selected();
    static bool exec_install(const std::string& cmd, std::string& log_out);
};

} // namespace straylight::wizard
