// apps/oobe/pages/welcome.cpp
// OOBE welcome page implementation
#include "welcome.h"

#include <imgui.h>

namespace straylight::oobe {

bool WelcomePage::render() {
    const ImGuiIO& io = ImGui::GetIO();
    float sw = io.DisplaySize.x;
    float sh = io.DisplaySize.y;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoCollapse;

    ImGui::Begin("##OobeWelcome", nullptr, flags);

    // StrayLight logo via ImDrawList (simplified geometric logo)
    ImDrawList* draw = ImGui::GetWindowDrawList();
    float cx = sw * 0.5f;
    float cy = sh * 0.3f;
    float size = 40.0f;

    // Draw a stylized 'S' hexagon shape
    ImU32 accent = ImGui::ColorConvertFloat4ToU32(
        ImVec4(0.7f, 0.75f, 1.0f, 1.0f));
    draw->AddNgonFilled(ImVec2(cx, cy), size, accent, 6);
    draw->AddNgon(ImVec2(cx, cy), size * 1.3f, accent, 6, 2.0f);

    // Headline
    ImGui::SetCursorPosY(sh * 0.45f);
    const char* headline = "Welcome to StrayLight OS";
    float hw = ImGui::CalcTextSize(headline).x;
    ImGui::SetCursorPosX((sw - hw) * 0.5f);
    ImGui::Text("%s", headline);

    // Subtext
    ImGui::Spacing();
    const char* subtext =
        "A high-performance Linux distribution built for ML workloads.";
    float stw = ImGui::CalcTextSize(subtext).x;
    ImGui::SetCursorPosX((sw - stw) * 0.5f);
    ImGui::TextDisabled("%s", subtext);

    ImGui::Spacing();
    ImGui::Spacing();

    // "Get Started" button
    float btn_w = 200.0f;
    float btn_h = 48.0f;
    ImGui::SetCursorPosX((sw - btn_w) * 0.5f);
    bool advance = ImGui::Button("Get Started", ImVec2(btn_w, btn_h));

    // Enter key also advances
    if (ImGui::IsKeyPressed(ImGuiKey_Enter)) {
        advance = true;
    }

    ImGui::End();

    return advance;
}

} // namespace straylight::oobe
