// apps/wizard/pages/theme_picker.cpp
// Theme picker page with live preview
#include "theme_picker.h"

#include <straylight/log.h>

#include <imgui.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace straylight::wizard {

namespace fs = std::filesystem;

struct ThemePreview {
    const char* name;
    ImVec4 bg;
    ImVec4 fg;
    ImVec4 accent;
    ImVec4 panel;
};

static const ThemePreview kThemes[] = {
    {"default",   {0.12f, 0.12f, 0.18f, 1.0f}, {0.80f, 0.84f, 0.96f, 1.0f},
                  {0.71f, 0.75f, 1.00f, 1.0f}, {0.19f, 0.20f, 0.27f, 1.0f}},
    {"cyberpunk", {0.05f, 0.05f, 0.10f, 1.0f}, {0.00f, 1.00f, 0.87f, 1.0f},
                  {1.00f, 0.00f, 0.50f, 1.0f}, {0.10f, 0.10f, 0.18f, 1.0f}},
    {"minimal",   {0.95f, 0.95f, 0.95f, 1.0f}, {0.10f, 0.10f, 0.10f, 1.0f},
                  {0.20f, 0.45f, 0.90f, 1.0f}, {0.88f, 0.88f, 0.88f, 1.0f}},
};
static constexpr int kNumThemes = 3;

bool ThemePickerPage::render() {
    const ImGuiIO& io = ImGui::GetIO();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse;

    ImGui::Begin("##WizardTheme", nullptr, flags);

    ImGui::SetCursorPosY(40.0f);
    ImGui::SetCursorPosX(40.0f);
    ImGui::Text("Choose Your Theme");
    ImGui::Separator();
    ImGui::Spacing();

    // Theme cards side by side
    float card_w = 220.0f;
    float card_h = 300.0f;
    float start_x = (io.DisplaySize.x - card_w * kNumThemes - 20.0f *
                     (kNumThemes - 1)) * 0.5f;

    for (int i = 0; i < kNumThemes; ++i) {
        float x = start_x + static_cast<float>(i) * (card_w + 20.0f);
        ImGui::SetCursorPos(ImVec2(x, 120.0f));

        bool is_selected = (i == selected_index_);
        const auto& theme = kThemes[i];

        // Push theme colors for preview
        ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.bg);
        ImGui::PushStyleColor(ImGuiCol_Text, theme.fg);
        ImGui::PushStyleColor(ImGuiCol_Border,
                              is_selected ? theme.accent
                                          : ImVec4(0.3f, 0.3f, 0.3f, 1.0f));

        char child_id[32];
        snprintf(child_id, sizeof(child_id), "##theme_%d", i);
        ImGui::BeginChild(child_id, ImVec2(card_w, card_h), true);

        // Theme name
        float nw = ImGui::CalcTextSize(theme.name).x;
        ImGui::SetCursorPosX((card_w - nw) * 0.5f);
        ImGui::Text("%s", theme.name);

        ImGui::Separator();

        // Mini preview: fake top bar + window
        ImDrawList* draw = ImGui::GetWindowDrawList();
        ImVec2 origin = ImGui::GetCursorScreenPos();

        // Top bar preview
        draw->AddRectFilled(
            origin,
            ImVec2(origin.x + card_w - 16.0f, origin.y + 16.0f),
            ImGui::ColorConvertFloat4ToU32(theme.panel));

        // Window preview
        draw->AddRectFilled(
            ImVec2(origin.x + 20.0f, origin.y + 24.0f),
            ImVec2(origin.x + card_w - 36.0f, origin.y + 120.0f),
            ImGui::ColorConvertFloat4ToU32(theme.panel));

        // Accent bar on window
        draw->AddRectFilled(
            ImVec2(origin.x + 20.0f, origin.y + 24.0f),
            ImVec2(origin.x + card_w - 36.0f, origin.y + 34.0f),
            ImGui::ColorConvertFloat4ToU32(theme.accent));

        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 140.0f);

        // Select button
        float bw = 100.0f;
        ImGui::SetCursorPosX((card_w - bw) * 0.5f);
        ImGui::PushStyleColor(ImGuiCol_Button, theme.accent);
        char btn_id[32];
        snprintf(btn_id, sizeof(btn_id), "Select##%d", i);
        if (ImGui::Button(btn_id, ImVec2(bw, 30.0f))) {
            selected_index_ = i;
            selected_ = theme.name;
        }
        ImGui::PopStyleColor();  // Button

        ImGui::EndChild();
        ImGui::PopStyleColor(3);  // ChildBg, Text, Border
    }

    // Next button
    ImGui::SetCursorPos(ImVec2(40.0f, io.DisplaySize.y - 80.0f));
    bool advance = false;
    if (ImGui::Button("Next", ImVec2(120, 40))) {
        // Write theme selection to config
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
            std::ofstream f(config_dir + "/theme.json", std::ios::trunc);
            f << "{\n  \"theme\": \"" << selected_ << "\"\n}\n";
            SL_INFO("Theme '{}' written to {}/theme.json",
                    selected_, config_dir);
        }
        advance = true;
    }

    ImGui::End();
    return advance;
}

} // namespace straylight::wizard
