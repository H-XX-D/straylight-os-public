// apps/wizard/pages/tour.cpp
// Interactive tour offer page
#include "tour.h"

#include <imgui.h>

#include <cmath>

namespace straylight::wizard {

bool TourPage::render() {
    const ImGuiIO& io = ImGui::GetIO();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove    | ImGuiWindowFlags_NoCollapse;

    ImGui::Begin("##WizardTour", nullptr, flags);

    // ── Animated glow behind title ────────────────────────────────────
    float t   = static_cast<float>(ImGui::GetTime());
    float glow = 0.55f + 0.45f * sinf(t * 1.4f);

    // Centre the content vertically ~40% down
    float centre_y = io.DisplaySize.y * 0.36f;

    // ── Headline ──────────────────────────────────────────────────────
    const char* headline = "You're all set!";
    ImVec2 hl_size = ImGui::CalcTextSize(headline);
    ImGui::SetCursorPos(ImVec2(
        (io.DisplaySize.x - hl_size.x) * 0.5f,
        centre_y));
    ImGui::PushStyleColor(ImGuiCol_Text,
        ImVec4(0.0f, glow, glow * 0.67f, 1.0f));
    ImGui::Text("%s", headline);
    ImGui::PopStyleColor();

    // ── Sub-text ──────────────────────────────────────────────────────
    const char* sub = "StrayLight is configured and ready to use.";
    ImVec2 sub_size = ImGui::CalcTextSize(sub);
    ImGui::SetCursorPos(ImVec2(
        (io.DisplaySize.x - sub_size.x) * 0.5f,
        centre_y + 44.0f));
    ImGui::TextDisabled("%s", sub);

    const char* sub2 = "Would you like a quick interactive tour of the desktop?";
    ImVec2 sub2_size = ImGui::CalcTextSize(sub2);
    ImGui::SetCursorPos(ImVec2(
        (io.DisplaySize.x - sub2_size.x) * 0.5f,
        centre_y + 80.0f));
    ImGui::TextUnformatted(sub2);

    // ── Buttons ───────────────────────────────────────────────────────
    constexpr float kBtnW = 180.0f;
    constexpr float kBtnH =  44.0f;
    constexpr float kGap  =  20.0f;
    float total_w = kBtnW * 2.0f + kGap;
    float btn_x   = (io.DisplaySize.x - total_w) * 0.5f;
    float btn_y   = centre_y + 148.0f;

    ImGui::SetCursorPos(ImVec2(btn_x, btn_y));
    ImGui::PushStyleColor(ImGuiCol_Button,
        ImVec4(0.0f, 0.55f * glow, 0.35f * glow, 1.0f));
    if (ImGui::Button("Yes, show me around!", ImVec2(kBtnW, kBtnH))) {
        choice_ = TourChoice::kYes;
        ImGui::PopStyleColor();
        ImGui::End();
        return true;
    }
    ImGui::PopStyleColor();

    ImGui::SameLine(0.0f, kGap);
    if (ImGui::Button("No thanks, go to desktop", ImVec2(kBtnW, kBtnH))) {
        choice_ = TourChoice::kNo;
        ImGui::End();
        return true;
    }

    // ── Tiny footer ──────────────────────────────────────────────────
    ImGui::SetCursorPos(ImVec2(0.0f, io.DisplaySize.y - 30.0f));
    ImGui::SetNextItemWidth(io.DisplaySize.x);
    ImGui::TextDisabled(
        "                          You can start the tour later from "
        "Settings → About StrayLight.");

    ImGui::End();
    return false;
}

} // namespace straylight::wizard
