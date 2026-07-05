// apps/wizard/pages/layout_config.cpp
// Layout configuration page implementation
#include "layout_config.h"

#include <straylight/log.h>

#include <imgui.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace straylight::wizard {

namespace fs = std::filesystem;

bool LayoutConfigPage::render() {
    const ImGuiIO& io = ImGui::GetIO();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse;

    ImGui::Begin("##WizardLayout", nullptr, flags);

    ImGui::SetCursorPosY(40.0f);
    ImGui::SetCursorPosX(40.0f);
    ImGui::Text("Desktop Layout");
    ImGui::Separator();
    ImGui::Spacing();

    // Toggle switches
    ImGui::SetCursorPosX(60.0f);
    ImGui::BeginDisabled(true);  // Top bar is always on
    ImGui::Checkbox("Top Bar (always enabled)", &top_bar_);
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::SetCursorPosX(60.0f);
    ImGui::Checkbox("Left Dock (64px, pinned apps)", &left_dock_);

    ImGui::Spacing();
    ImGui::SetCursorPosX(60.0f);
    ImGui::Checkbox("Bottom Dock (48px, task bar)", &bottom_dock_);

    ImGui::Spacing();
    ImGui::Spacing();

    // Live preview diagram
    ImGui::SetCursorPosX(60.0f);
    ImGui::Text("Preview");

    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 origin = ImVec2(60.0f + ImGui::GetWindowPos().x,
                           ImGui::GetCursorScreenPos().y + 10.0f);
    float pw = 400.0f;
    float ph = 280.0f;

    // Screen outline
    draw->AddRect(origin, ImVec2(origin.x + pw, origin.y + ph),
                  IM_COL32(128, 128, 128, 255), 4.0f, 0, 2.0f);

    // Top bar
    ImU32 active = IM_COL32(100, 120, 200, 200);
    ImU32 inactive = IM_COL32(60, 60, 60, 100);

    draw->AddRectFilled(
        origin, ImVec2(origin.x + pw, origin.y + 20.0f),
        top_bar_ ? active : inactive);

    // Left dock
    if (left_dock_) {
        draw->AddRectFilled(
            ImVec2(origin.x, origin.y + 20.0f),
            ImVec2(origin.x + 40.0f, origin.y + ph),
            active);
    }

    // Bottom dock
    if (bottom_dock_) {
        draw->AddRectFilled(
            ImVec2(origin.x, origin.y + ph - 24.0f),
            ImVec2(origin.x + pw, origin.y + ph),
            active);
    }

    // Next button
    ImGui::SetCursorPos(ImVec2(40.0f, io.DisplaySize.y - 80.0f));
    bool advance = false;
    if (ImGui::Button("Next", ImVec2(120, 40))) {
        // Write layout config
        const char* home = std::getenv("HOME");
        const char* xdg = std::getenv("XDG_CONFIG_HOME");
        std::string config_dir;
        if (xdg) {
            config_dir = std::string(xdg) + "/straylight";
        } else if (home) {
            config_dir = std::string(home) + "/.config/straylight";
        }

        if (!config_dir.empty()) {
            std::error_code ec;
            fs::create_directories(config_dir, ec);
            std::ofstream f(config_dir + "/shell.json", std::ios::trunc);
            f << "{\n"
              << "  \"top_bar\": " << (top_bar_ ? "true" : "false") << ",\n"
              << "  \"left_dock\": " << (left_dock_ ? "true" : "false") << ",\n"
              << "  \"bottom_dock\": " << (bottom_dock_ ? "true" : "false") << "\n"
              << "}\n";
            SL_INFO("Layout config written: left_dock={}, bottom_dock={}",
                    left_dock_, bottom_dock_);
        }
        advance = true;
    }

    ImGui::End();
    return advance;
}

} // namespace straylight::wizard
