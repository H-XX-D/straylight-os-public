// apps/settings/pages/appearance.cpp
// Appearance settings — themes, fonts, accent color
#include "appearance.h"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace straylight::settings {

using json = nlohmann::json;
namespace fs = std::filesystem;

AppearancePage::AppearancePage() {
    init_themes();
}

void AppearancePage::init_themes() {
    themes_.clear();

    // Cyberpunk (default)
    {
        Theme t;
        t.name = "cyberpunk";
        t.description = "Dark cyberpunk theme with neon accents";
        t.colors.bg_primary = 0xFF1A1A2E;
        t.colors.bg_secondary = 0xFF16213E;
        t.colors.bg_tertiary = 0xFF0F3460;
        t.colors.fg_primary = 0xFFCCCCCC;
        t.colors.fg_secondary = 0xFF888888;
        t.colors.accent = 0xFF00FFAA;
        t.colors.error = 0xFFE74C3C;
        t.colors.warning = 0xFFF39C12;
        t.colors.success = 0xFF2ECC71;
        themes_.push_back(std::move(t));
    }

    // Default (standard dark)
    {
        Theme t;
        t.name = "default";
        t.description = "Standard dark theme";
        t.colors.bg_primary = 0xFF1E1E1E;
        t.colors.bg_secondary = 0xFF252526;
        t.colors.bg_tertiary = 0xFF333333;
        t.colors.fg_primary = 0xFFD4D4D4;
        t.colors.fg_secondary = 0xFF808080;
        t.colors.accent = 0xFF569CD6;
        t.colors.error = 0xFFF44747;
        t.colors.warning = 0xFFE2C08D;
        t.colors.success = 0xFF6A9955;
        themes_.push_back(std::move(t));
    }

    // Minimal (light)
    {
        Theme t;
        t.name = "minimal";
        t.description = "Clean minimal light theme";
        t.colors.bg_primary = 0xFFF5F5F5;
        t.colors.bg_secondary = 0xFFFFFFFF;
        t.colors.bg_tertiary = 0xFFE0E0E0;
        t.colors.fg_primary = 0xFF333333;
        t.colors.fg_secondary = 0xFF666666;
        t.colors.accent = 0xFF0066CC;
        t.colors.error = 0xFFCC0000;
        t.colors.warning = 0xFFCC8800;
        t.colors.success = 0xFF008800;
        themes_.push_back(std::move(t));
    }
}

static uint32_t hex_to_u32(const std::string& s) {
    if (s.size() >= 7 && s[0] == '#') {
        unsigned long v = std::stoul(s.substr(1), nullptr, 16);
        if (s.size() == 7) return 0xFF000000u | static_cast<uint32_t>(v);
        return static_cast<uint32_t>(v);
    }
    return 0xFFCCCCCC;
}

void AppearancePage::load() {
    std::vector<std::string> paths;
    const char* xdg = getenv("XDG_CONFIG_HOME");
    const char* home = getenv("HOME");
    if (xdg) paths.push_back(std::string(xdg) + "/straylight/appearance.json");
    if (home) paths.push_back(std::string(home) + "/.config/straylight/appearance.json");

    for (const auto& path : paths) {
        std::ifstream file(path);
        if (!file.is_open()) continue;

        json j;
        try { file >> j; } catch (...) { continue; }

        if (j.contains("theme")) settings_.theme_name = j["theme"].get<std::string>();
        if (j.contains("font_family")) settings_.font_family = j["font_family"].get<std::string>();
        if (j.contains("font_size")) settings_.font_size = j["font_size"].get<float>();
        if (j.contains("accent_color")) {
            auto& ac = j["accent_color"];
            if (ac.is_string()) settings_.accent_color = hex_to_u32(ac.get<std::string>());
            else if (ac.is_number()) settings_.accent_color = ac.get<uint32_t>();
        }
        if (j.contains("animations")) settings_.animations_enabled = j["animations"].get<bool>();
        if (j.contains("window_opacity")) settings_.window_opacity = j["window_opacity"].get<float>();
        if (j.contains("border_radius")) settings_.border_radius = j["border_radius"].get<float>();

        break;
    }
}

Result<void, std::string> AppearancePage::apply() {
    // Find and apply the selected theme
    for (const auto& theme : themes_) {
        if (theme.name == settings_.theme_name) {
            apply_theme(theme);
            break;
        }
    }

    // Write runtime config
    json config;
    config["theme"] = settings_.theme_name;
    config["font_family"] = settings_.font_family;
    config["font_size"] = settings_.font_size;
    config["accent_color"] = settings_.accent_color;
    config["animations"] = settings_.animations_enabled;
    config["window_opacity"] = settings_.window_opacity;
    config["border_radius"] = settings_.border_radius;

    std::ofstream file("/tmp/straylight-appearance-config.json");
    if (!file.is_open()) {
        return Result<void, std::string>::error("Cannot write appearance config");
    }
    file << config.dump(2);

    dirty_ = false;
    return Result<void, std::string>::ok();
}

void AppearancePage::apply_theme(const Theme& theme) {
    ImGuiStyle& style = ImGui::GetStyle();

    auto to_imvec4 = [](uint32_t argb) -> ImVec4 {
        float a = static_cast<float>((argb >> 24) & 0xFF) / 255.0f;
        float r = static_cast<float>((argb >> 16) & 0xFF) / 255.0f;
        float g = static_cast<float>((argb >> 8) & 0xFF) / 255.0f;
        float b = static_cast<float>(argb & 0xFF) / 255.0f;
        return ImVec4(r, g, b, a);
    };

    style.Colors[ImGuiCol_WindowBg] = to_imvec4(theme.colors.bg_primary);
    style.Colors[ImGuiCol_ChildBg] = to_imvec4(theme.colors.bg_secondary);
    style.Colors[ImGuiCol_PopupBg] = to_imvec4(theme.colors.bg_secondary);
    style.Colors[ImGuiCol_FrameBg] = to_imvec4(theme.colors.bg_tertiary);
    style.Colors[ImGuiCol_FrameBgHovered] = to_imvec4(theme.colors.accent);
    style.Colors[ImGuiCol_Text] = to_imvec4(theme.colors.fg_primary);
    style.Colors[ImGuiCol_TextDisabled] = to_imvec4(theme.colors.fg_secondary);
    style.Colors[ImGuiCol_Header] = to_imvec4(theme.colors.bg_tertiary);
    style.Colors[ImGuiCol_HeaderHovered] = to_imvec4(theme.colors.accent);
    style.Colors[ImGuiCol_Button] = to_imvec4(theme.colors.bg_tertiary);
    style.Colors[ImGuiCol_ButtonHovered] = to_imvec4(theme.colors.accent);
    style.Colors[ImGuiCol_Tab] = to_imvec4(theme.colors.bg_secondary);
    style.Colors[ImGuiCol_TabHovered] = to_imvec4(theme.colors.accent);

    style.WindowRounding = settings_.border_radius;
    style.FrameRounding = settings_.border_radius * 0.5f;
}

Result<void, std::string> AppearancePage::save() {
    const char* home = getenv("HOME");
    if (!home) return Result<void, std::string>::error("HOME not set");

    std::string dir = std::string(home) + "/.config/straylight";
    std::error_code ec;
    fs::create_directories(dir, ec);

    json j;
    j["theme"] = settings_.theme_name;
    j["font_family"] = settings_.font_family;
    j["font_size"] = settings_.font_size;
    j["accent_color"] = settings_.accent_color;
    j["animations"] = settings_.animations_enabled;
    j["window_opacity"] = settings_.window_opacity;
    j["border_radius"] = settings_.border_radius;

    std::ofstream file(dir + "/appearance.json");
    if (!file.is_open()) {
        return Result<void, std::string>::error("Cannot save appearance config");
    }
    file << j.dump(2);

    return Result<void, std::string>::ok();
}

void AppearancePage::render() {
    ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.67f, 1.0f), "Appearance");
    ImGui::Separator();

    // Theme selection
    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Theme");

    for (const auto& theme : themes_) {
        bool selected = (theme.name == settings_.theme_name);

        ImGui::PushID(theme.name.c_str());

        // Theme preview box
        auto to_imvec4 = [](uint32_t argb) -> ImVec4 {
            float a = static_cast<float>((argb >> 24) & 0xFF) / 255.0f;
            float r = static_cast<float>((argb >> 16) & 0xFF) / 255.0f;
            float g = static_cast<float>((argb >> 8) & 0xFF) / 255.0f;
            float b = static_cast<float>(argb & 0xFF) / 255.0f;
            return ImVec4(r, g, b, a);
        };

        ImGui::BeginGroup();

        // Color preview swatches
        ImVec2 swatch_size(20, 20);
        ImGui::ColorButton("##bg", to_imvec4(theme.colors.bg_primary),
                           ImGuiColorEditFlags_NoTooltip, swatch_size);
        ImGui::SameLine();
        ImGui::ColorButton("##accent", to_imvec4(theme.colors.accent),
                           ImGuiColorEditFlags_NoTooltip, swatch_size);
        ImGui::SameLine();
        ImGui::ColorButton("##fg", to_imvec4(theme.colors.fg_primary),
                           ImGuiColorEditFlags_NoTooltip, swatch_size);
        ImGui::SameLine();

        if (ImGui::Selectable(theme.name.c_str(), selected, 0,
                              ImVec2(200, 20))) {
            settings_.theme_name = theme.name;
            dirty_ = true;
        }
        ImGui::Text("  %s", theme.description.c_str());

        ImGui::EndGroup();
        ImGui::PopID();
        ImGui::Spacing();
    }

    ImGui::Separator();

    // Accent color picker
    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Accent Color");
    float col[4] = {
        static_cast<float>((settings_.accent_color >> 16) & 0xFF) / 255.0f,
        static_cast<float>((settings_.accent_color >> 8) & 0xFF) / 255.0f,
        static_cast<float>(settings_.accent_color & 0xFF) / 255.0f,
        static_cast<float>((settings_.accent_color >> 24) & 0xFF) / 255.0f,
    };

    if (ImGui::ColorEdit3("##AccentColor", col)) {
        settings_.accent_color = 0xFF000000u |
            (static_cast<uint32_t>(col[0] * 255.0f) << 16) |
            (static_cast<uint32_t>(col[1] * 255.0f) << 8) |
            static_cast<uint32_t>(col[2] * 255.0f);
        dirty_ = true;
    }

    // Quick accent presets
    struct PresetColor { const char* name; uint32_t color; };
    PresetColor presets[] = {
        {"Neon Green", 0xFF00FFAA},
        {"Neon Blue", 0xFF00AAFF},
        {"Neon Pink", 0xFFFF00AA},
        {"Neon Yellow", 0xFFFFDD00},
        {"Classic Blue", 0xFF3498DB},
        {"Sunset Orange", 0xFFE74C3C},
    };

    for (const auto& [name, color] : presets) {
        float pc[4] = {
            static_cast<float>((color >> 16) & 0xFF) / 255.0f,
            static_cast<float>((color >> 8) & 0xFF) / 255.0f,
            static_cast<float>(color & 0xFF) / 255.0f,
            1.0f,
        };
        ImGui::SameLine();
        if (ImGui::ColorButton(name, ImVec4(pc[0], pc[1], pc[2], pc[3]),
                               ImGuiColorEditFlags_NoTooltip, ImVec2(25, 25))) {
            settings_.accent_color = color;
            dirty_ = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", name);
        }
    }

    ImGui::Separator();

    // Font settings
    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Font");

    ImGui::Text("Font Size:");
    if (ImGui::SliderFloat("##FontSize", &settings_.font_size, 8.0f, 24.0f,
                            "%.0f")) {
        dirty_ = true;
    }

    ImGui::Separator();

    // Window settings
    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Window");

    ImGui::Text("Window Opacity:");
    if (ImGui::SliderFloat("##WinOpacity", &settings_.window_opacity,
                            0.5f, 1.0f, "%.2f")) {
        dirty_ = true;
    }

    ImGui::Text("Border Radius:");
    if (ImGui::SliderFloat("##BorderRadius", &settings_.border_radius,
                            0.0f, 16.0f, "%.0f")) {
        dirty_ = true;
    }

    if (ImGui::Checkbox("Enable Animations", &settings_.animations_enabled)) {
        dirty_ = true;
    }

    ImGui::Spacing();
    ImGui::Separator();

    if (dirty_) {
        if (ImGui::Button("Apply", ImVec2(120, 30))) {
            apply();
            save();
        }
    }
}

} // namespace straylight::settings
