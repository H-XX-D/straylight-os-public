// apps/greeter/ui.cpp
// Greeter login UI implementation
#include "ui.h"

#include <straylight/log.h>

#include <imgui.h>

#include <cmath>
#include <cstring>

namespace straylight::greeter {

static const char* kSessionTypes[] = {"straylight", "tty"};
static constexpr int kNumSessions = 2;

GreeterUI::GreeterUI() = default;
GreeterUI::~GreeterUI() = default;

bool GreeterUI::render() {
    submitted_ = false;

    const ImGuiIO& io = ImGui::GetIO();
    float sw = io.DisplaySize.x;
    float sh = io.DisplaySize.y;

    render_background();
    render_login_card(sw, sh);

    return submitted_;
}

void GreeterUI::render_background() {
    const ImGuiIO& io = ImGui::GetIO();

    // Fullscreen background window
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoInputs;

    ImGui::Begin("##Background", nullptr, flags);

    // Subtle gradient pulse animation (4s cycle)
    float t = static_cast<float>(ImGui::GetTime());
    float pulse = 0.05f + 0.02f * std::sin(t * 1.5708f);  // pi/2 rad/s ~ 4s

    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImU32 color_top = ImGui::ColorConvertFloat4ToU32(
        ImVec4(pulse, pulse, pulse * 1.5f, 1.0f));
    ImU32 color_bottom = ImGui::ColorConvertFloat4ToU32(
        ImVec4(0.02f, 0.02f, 0.05f, 1.0f));

    draw->AddRectFilledMultiColor(
        ImVec2(0, 0), io.DisplaySize,
        color_top, color_top, color_bottom, color_bottom);

    ImGui::End();
}

void GreeterUI::render_login_card(float screen_width, float screen_height) {
    constexpr float kCardWidth  = 480.0f;
    constexpr float kCardHeight = 320.0f;

    float card_x = (screen_width  - kCardWidth)  * 0.5f;
    float card_y = (screen_height - kCardHeight) * 0.5f;

    ImGui::SetNextWindowPos(ImVec2(card_x, card_y));
    ImGui::SetNextWindowSize(ImVec2(kCardWidth, kCardHeight));

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse;

    ImGui::Begin("##LoginCard", nullptr, flags);

    // Title
    ImGui::SetCursorPosY(20.0f);
    float title_w = ImGui::CalcTextSize("StrayLight OS").x;
    ImGui::SetCursorPosX((kCardWidth - title_w) * 0.5f);
    ImGui::Text("StrayLight OS");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Username field
    ImGui::Text("Username");
    ImGui::SetNextItemWidth(kCardWidth - 40.0f);
    bool enter_pressed = ImGui::InputText(
        "##username", username_buf_, sizeof(username_buf_),
        ImGuiInputTextFlags_EnterReturnsTrue);

    ImGui::Spacing();

    // Password field (masked)
    ImGui::Text("Password");
    ImGui::SetNextItemWidth(kCardWidth - 40.0f);
    enter_pressed |= ImGui::InputText(
        "##password", password_buf_, sizeof(password_buf_),
        ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);

    ImGui::Spacing();

    // Session selector
    ImGui::Text("Session");
    ImGui::SetNextItemWidth(150.0f);
    ImGui::Combo("##session", &session_index_,
                 kSessionTypes, kNumSessions);
    selected_session_ = kSessionTypes[session_index_];

    ImGui::Spacing();

    // Login button
    float btn_w = 120.0f;
    ImGui::SetCursorPosX((kCardWidth - btn_w) * 0.5f);
    if (ImGui::Button("Login", ImVec2(btn_w, 36.0f)) || enter_pressed) {
        username_ = username_buf_;
        password_ = password_buf_;
        submitted_ = true;
        SL_DEBUG("Login submitted for user '{}'", username_);
    }

    // Error banner
    if (!error_message_.empty()) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
        ImGui::TextWrapped("%s", error_message_.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::End();
}

const std::string& GreeterUI::username() const {
    return username_;
}

const std::string& GreeterUI::password() const {
    return password_;
}

void GreeterUI::set_error(const std::string& message) {
    error_message_ = message;
}

void GreeterUI::clear_error() {
    error_message_.clear();
}

const std::string& GreeterUI::selected_session() const {
    return selected_session_;
}

bool GreeterUI::has_error() const {
    return !error_message_.empty();
}

} // namespace straylight::greeter
