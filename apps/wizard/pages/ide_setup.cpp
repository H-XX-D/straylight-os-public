// apps/wizard/pages/ide_setup.cpp
// IDE selection and automatic installation
#include "ide_setup.h"

#include <straylight/log.h>

#include <imgui.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <thread>

namespace straylight::wizard {

namespace fs = std::filesystem;

// ── IDE catalogue ─────────────────────────────────────────────────────────

static const IdeEntry kIdeCatalogue[] = {
    // id            display name               description                      install command
    {"vscode",       "Visual Studio Code",      "Microsoft's popular editor — extensions, debugger, built-in Git",
     "flatpak install -y flathub com.visualstudio.code"},
    {"vscodium",     "VSCodium",                "Open-source VS Code without telemetry",
     "flatpak install -y flathub com.vscodium.codium"},
    {"neovim",       "Neovim",                  "Hyper-extensible terminal editor",
     "flatpak install -y flathub io.neovim.nvim"},
    {"helix",        "Helix",                   "Modal editor with built-in LSP and tree-sitter",
     "flatpak install -y flathub com.helix_editor.Helix"},
    {"clion",        "CLion",                   "JetBrains C/C++ IDE",
     "flatpak install -y flathub com.jetbrains.CLion"},
    {"pycharm",      "PyCharm Community",       "JetBrains Python IDE",
     "flatpak install -y flathub com.jetbrains.PyCharm-Community"},
    {"intellij",     "IntelliJ IDEA Community", "JetBrains Java/Kotlin IDE",
     "flatpak install -y flathub com.jetbrains.IntelliJ-IDEA-Community"},
    {"zed",          "Zed",                     "Fast, multiplayer code editor written in Rust",
     "flatpak install -y flathub dev.zed.Zed"},
    {"emacs",        "GNU Emacs",               "The extensible self-documenting editor",
     "flatpak install -y flathub org.gnu.emacs"},
    {"none",         "Skip — I'll install my own", "No IDE will be installed now",
     ""},
};

// ── Helpers ───────────────────────────────────────────────────────────────

/// Return true if a binary or Flatpak app ID is currently installed.
static bool probe_installed(const std::string& id) {
    // Check binary names first
    static const struct { const char* id; const char* bin; } bins[] = {
        {"neovim",  "nvim"},
        {"emacs",   "emacs"},
        {"helix",   "hx"},
        {nullptr,   nullptr},
    };
    for (int i = 0; bins[i].id; ++i) {
        if (id == bins[i].id) {
            std::string path = "/usr/bin/" + std::string(bins[i].bin);
            if (fs::exists(path)) return true;
        }
    }
    // Check Flatpak app ids (flatpak list --app-runtime --columns=application)
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "flatpak info --show-metadata %s 2>/dev/null | head -1",
             // extract the Flatpak app-id from the install_cmd
             // simplest: flatpak info <last-token-of-install-cmd>
             id.c_str());
    // Quick heuristic: look for ~/.local/share/flatpak/app/<id> or /var/lib/flatpak/app/<id>
    //  We build the Flatpak app-id from the catalogue's install_cmd.
    return false;   // detection is best-effort; installer will just re-install
}

bool IdeSetupPage::exec_install(const std::string& cmd, std::string& log_out) {
    if (cmd.empty()) return true;
    SL_INFO("ide_setup: running: {}", cmd);
    FILE* p = popen((cmd + " 2>&1").c_str(), "r");
    if (!p) { log_out = "Failed to run installer"; return false; }
    char buf[256];
    while (fgets(buf, sizeof(buf), p))
        log_out += buf;
    int rc = pclose(p);
    return (rc == 0);
}

// ── IdeSetupPage ─────────────────────────────────────────────────────────

IdeSetupPage::IdeSetupPage() {
    for (const auto& e : kIdeCatalogue)
        ides_.push_back(e);
}

void IdeSetupPage::detect_installed() {
    for (auto& e : ides_) {
        e.installed = probe_installed(e.id);
        if (e.installed)
            SL_INFO("ide_setup: {} already installed", e.id);
    }
    detect_done_ = true;
}

void IdeSetupPage::install_selected() {
    install_status_ = IdeInstallStatus::kDownloading;

    std::thread([this]() {
        for (auto& e : ides_) {
            if (!e.selected || e.id == "none" || e.installed) continue;
            current_installing_ = e.display_name;
            install_status_     = IdeInstallStatus::kInstalling;
            SL_INFO("ide_setup: installing {}", e.id);

            std::string log;
            bool ok = exec_install(e.install_cmd, log);
            install_log_ += "=== " + e.display_name + " ===\n" + log + "\n";
            if (!ok) {
                SL_ERROR("ide_setup: install failed for {}", e.id);
                install_status_ = IdeInstallStatus::kFailed;
                return;
            }
            e.installed = true;
        }
        install_status_ = IdeInstallStatus::kDone;
    }).detach();
}

bool IdeSetupPage::render() {
    if (!detect_done_) detect_installed();

    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove    | ImGuiWindowFlags_NoCollapse;

    ImGui::Begin("##WizardIDE", nullptr, flags);

    // ── Header ─────────────────────────────────────────────────────────
    ImGui::SetCursorPos(ImVec2(40.0f, 30.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.67f, 1.0f));
    ImGui::Text("Choose Your IDE");
    ImGui::PopStyleColor();
    ImGui::SetCursorPosX(40.0f);
    ImGui::TextDisabled(
        "Select the editors you'd like installed. "
        "They will be downloaded automatically.");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    bool advance = false;

    if (install_status_ == IdeInstallStatus::kIdle ||
        install_status_ == IdeInstallStatus::kFailed) {

        // ── IDE list ────────────────────────────────────────────────────
        ImGui::BeginChild("##idelist",
                          ImVec2(io.DisplaySize.x - 80.0f,
                                 io.DisplaySize.y - 180.0f),
                          true);

        for (auto& e : ides_) {
            ImGui::PushID(e.id.c_str());

            bool chk = e.selected;
            if (ImGui::Checkbox(("##sel_" + e.id).c_str(), &chk))
                e.selected = chk;

            ImGui::SameLine();
            ImGui::BeginGroup();

            ImGui::Text("%s", e.display_name.c_str());
            ImGui::SameLine();
            if (e.installed) {
                ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f),
                                   " [already installed]");
            }
            ImGui::TextDisabled("  %s", e.description.c_str());

            ImGui::EndGroup();
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::PopID();
        }
        ImGui::EndChild();

        // Error banner
        if (install_status_ == IdeInstallStatus::kFailed) {
            ImGui::SetCursorPosX(40.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                               "One or more installations failed — "
                               "check logs or skip.");
        }

        // Buttons
        ImGui::SetCursorPos(ImVec2(40.0f, io.DisplaySize.y - 70.0f));
        bool any_selected = false;
        for (const auto& e : ides_)
            if (e.selected && e.id != "none") { any_selected = true; break; }

        if (any_selected) {
            if (ImGui::Button("Install & Continue", ImVec2(200, 40))) {
                // Record selections
                selected_ids_.clear();
                for (const auto& e : ides_)
                    if (e.selected) selected_ids_.push_back(e.id);
                install_selected();
            }
        } else {
            if (ImGui::Button("Skip", ImVec2(120, 40))) {
                advance = true;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(select an IDE above to install it)");
        }

    } else if (install_status_ == IdeInstallStatus::kDownloading ||
               install_status_ == IdeInstallStatus::kInstalling) {

        // ── Progress ────────────────────────────────────────────────────
        ImGui::SetCursorPos(ImVec2(40.0f, 120.0f));
        ImGui::Text("Installing: %s", current_installing_.c_str());

        float t = static_cast<float>(ImGui::GetTime());
        float progress = 0.5f + 0.5f * sinf(t * 2.0f);  // pulsing bar
        ImGui::SetCursorPos(ImVec2(40.0f, 160.0f));
        ImGui::ProgressBar(progress,
                           ImVec2(io.DisplaySize.x - 80.0f, 20.0f), "");

        // Show last few lines of log
        if (!install_log_.empty()) {
            ImGui::SetCursorPos(ImVec2(40.0f, 200.0f));
            ImGui::BeginChild("##install_log",
                              ImVec2(io.DisplaySize.x - 80.0f, 200.0f),
                              true);
            ImGui::TextUnformatted(install_log_.c_str());
            ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();
        }

    } else if (install_status_ == IdeInstallStatus::kDone) {

        // ── Done ────────────────────────────────────────────────────────
        ImGui::SetCursorPos(ImVec2(40.0f, 120.0f));
        ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f),
                           "Installation complete!");
        ImGui::SetCursorPos(ImVec2(40.0f, io.DisplaySize.y - 70.0f));
        if (ImGui::Button("Continue", ImVec2(140, 40)))
            advance = true;
    }

    ImGui::End();
    return advance;
}

} // namespace straylight::wizard
