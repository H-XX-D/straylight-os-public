// apps/wizard/pages/summary.cpp
// Wizard summary page implementation
#include "summary.h"

#include <straylight/log.h>

#include <imgui.h>

namespace straylight::wizard {

void SummaryPage::set_theme(const std::string& theme) {
    theme_ = theme;
}

void SummaryPage::set_layout(const std::string& layout) {
    layout_ = layout;
}

void SummaryPage::set_gpu_profile(const std::string& profile) {
    gpu_profile_ = profile;
}

bool SummaryPage::render() {
    const ImGuiIO& io = ImGui::GetIO();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse;

    ImGui::Begin("##WizardSummary", nullptr, flags);

    ImGui::SetCursorPosY(40.0f);
    ImGui::SetCursorPosX(40.0f);
    ImGui::Text("Setup Complete");
    ImGui::Separator();
    ImGui::Spacing();

    // Summary display
    ImGui::SetCursorPosX(60.0f);
    ImGui::Text("Theme:");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.7f, 0.75f, 1.0f, 1.0f),
                       "%s", theme_.c_str());

    ImGui::Spacing();
    ImGui::SetCursorPosX(60.0f);
    ImGui::Text("Layout:");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.7f, 0.75f, 1.0f, 1.0f),
                       "%s", layout_.c_str());

    ImGui::Spacing();
    ImGui::SetCursorPosX(60.0f);
    ImGui::Text("GPU Profile:");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.7f, 0.75f, 1.0f, 1.0f),
                       "%s", gpu_profile_.c_str());

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::SetCursorPosX(60.0f);
    ImGui::TextWrapped(
        "Your preferences will be applied to the desktop shell. "
        "You can change these later in Settings > Personalization.");

    // Finish button
    ImGui::SetCursorPos(ImVec2(40.0f, io.DisplaySize.y - 80.0f));
    bool finish = false;
    if (ImGui::Button("Finish", ImVec2(120, 40))) {
        SL_INFO("Wizard complete: theme={}, layout={}, gpu={}",
                theme_, layout_, gpu_profile_);
        // Theme preferences are persisted for the distro desktop session.
        finish = true;
    }

    ImGui::End();
    return finish;
}

} // namespace straylight::wizard
