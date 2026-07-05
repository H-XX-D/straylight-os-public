// apps/oobe/pages/summary.cpp
// OOBE summary page implementation
#include "summary.h"

#include <straylight/log.h>

#include <imgui.h>

namespace straylight::oobe {

void SummaryPage::set_user_info(const std::string& info) {
    user_info_ = info;
}

void SummaryPage::set_profile_info(const std::string& info) {
    profile_info_ = info;
}

void SummaryPage::set_network_info(const std::string& info) {
    network_info_ = info;
}

int SummaryPage::render() {
    const ImGuiIO& io = ImGui::GetIO();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse;

    ImGui::Begin("##OobeSummary", nullptr, flags);

    ImGui::SetCursorPosY(60.0f);
    ImGui::SetCursorPosX(60.0f);
    ImGui::Text("Setup Summary");
    ImGui::Separator();
    ImGui::Spacing();

    // User section
    ImGui::SetCursorPosX(60.0f);
    ImGui::Text("User Account");
    ImGui::SetCursorPosX(80.0f);
    ImGui::TextWrapped("%s", user_info_.c_str());
    ImGui::Spacing();

    // Profile section
    ImGui::SetCursorPosX(60.0f);
    ImGui::Text("Package Profile");
    ImGui::SetCursorPosX(80.0f);
    ImGui::TextWrapped("%s", profile_info_.c_str());
    ImGui::Spacing();

    // Network section
    ImGui::SetCursorPosX(60.0f);
    ImGui::Text("Network");
    ImGui::SetCursorPosX(80.0f);
    ImGui::TextWrapped("%s", network_info_.c_str());
    ImGui::Spacing();
    ImGui::Spacing();

    int result = 0;

    ImGui::SetCursorPosX(60.0f);
    if (ImGui::Button("Apply and Continue", ImVec2(200, 40))) {
        SL_INFO("OOBE summary: applying changes and transitioning to wizard");
        result = 1;
    }
    ImGui::SameLine();
    if (ImGui::Button("Back", ImVec2(80, 40))) {
        result = -1;
    }

    ImGui::End();
    return result;
}

} // namespace straylight::oobe
