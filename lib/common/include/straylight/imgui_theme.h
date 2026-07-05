#pragma once

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace straylight::ui {

struct ThemeSpec {
    std::string id = "straylight-nexus";
    std::string name = "Nexus";
    ImVec4 bg = ImVec4(0.039f, 0.055f, 0.090f, 1.0f);
    ImVec4 panel = ImVec4(0.075f, 0.102f, 0.169f, 1.0f);
    ImVec4 panel_raised = ImVec4(0.118f, 0.153f, 0.251f, 1.0f);
    ImVec4 fg = ImVec4(0.886f, 0.910f, 0.941f, 1.0f);
    ImVec4 muted = ImVec4(0.580f, 0.639f, 0.722f, 1.0f);
    ImVec4 accent = ImVec4(0.000f, 0.831f, 0.667f, 1.0f);
    ImVec4 accent_2 = ImVec4(0.388f, 0.400f, 0.945f, 1.0f);
    ImVec4 border = ImVec4(0.118f, 0.161f, 0.231f, 1.0f);
    ImVec4 success = ImVec4(0.063f, 0.725f, 0.506f, 1.0f);
    ImVec4 warning = ImVec4(0.961f, 0.620f, 0.043f, 1.0f);
    ImVec4 error = ImVec4(0.937f, 0.267f, 0.267f, 1.0f);
    float font_size = 15.0f;
    float radius = 8.0f;
};

inline std::string theme_id_from_path(const std::filesystem::path& path) {
    return path.stem().string();
}

inline std::optional<nlohmann::json> read_json(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) return std::nullopt;
    try {
        nlohmann::json data;
        file >> data;
        return data;
    } catch (...) {
        return std::nullopt;
    }
}

inline const nlohmann::json* json_at(const nlohmann::json& root,
                                     std::initializer_list<const char*> keys) {
    const nlohmann::json* current = &root;
    for (const char* key : keys) {
        if (!current->is_object() || !current->contains(key)) return nullptr;
        current = &(*current)[key];
    }
    return current;
}

inline std::string json_string(const nlohmann::json& root,
                               std::initializer_list<const char*> keys,
                               std::string fallback) {
    const nlohmann::json* value = json_at(root, keys);
    return value && value->is_string() ? value->get<std::string>() : fallback;
}

inline float json_float(const nlohmann::json& root,
                        std::initializer_list<const char*> keys,
                        float fallback) {
    const nlohmann::json* value = json_at(root, keys);
    if (!value) return fallback;
    if (value->is_number()) return value->get<float>();
    if (value->is_string()) {
        try { return std::stof(value->get<std::string>()); } catch (...) {}
    }
    return fallback;
}

inline ImVec4 with_alpha(ImVec4 color, float alpha) {
    color.w = alpha;
    return color;
}

inline ImVec4 hex_color(std::string value, ImVec4 fallback) {
    if (value.empty()) return fallback;
    if (value[0] == '#') value.erase(value.begin());
    if (value.size() != 6 && value.size() != 8) return fallback;
    try {
        const auto channel = [&](std::size_t offset) -> float {
            return static_cast<float>(std::stoul(value.substr(offset, 2), nullptr, 16)) / 255.0f;
        };
        float alpha = value.size() == 8 ? channel(6) : 1.0f;
        return ImVec4(channel(0), channel(2), channel(4), alpha);
    } catch (...) {
        return fallback;
    }
}

inline std::filesystem::path theme_path(std::string_view id) {
    std::filesystem::path raw{id};
    if (raw.is_absolute() && std::filesystem::exists(raw)) return raw;
    std::filesystem::path configured = std::filesystem::path("/etc/straylight/themes") /
                                      (std::string(id) + ".json");
    if (std::filesystem::exists(configured)) return configured;
    return "/etc/straylight/themes/straylight-nexus.json";
}

inline std::string configured_theme_id() {
    if (const char* env = std::getenv("STRAYLIGHT_THEME"); env && *env) return env;
    if (const char* home = std::getenv("HOME"); home && *home) {
        auto user = read_json(std::filesystem::path(home) / ".config/straylight/theme.json");
        if (user) {
            std::string direct = json_string(*user, {"theme"}, "");
            if (!direct.empty()) return direct;
            std::string nested = json_string(*user, {"shell", "theme"}, "");
            if (!nested.empty()) return nested;
        }
    }
    auto system = read_json("/etc/straylight/theme.json");
    if (system) {
        std::string nested = json_string(*system, {"shell", "theme"}, "");
        if (!nested.empty()) return nested;
        std::string direct = json_string(*system, {"theme"}, "");
        if (!direct.empty()) return direct;
    }
    return "straylight-nexus";
}

inline ThemeSpec load_theme(const std::filesystem::path& path) {
    ThemeSpec theme;
    theme.id = theme_id_from_path(path);
    auto data = read_json(path);
    if (!data) return theme;

    theme.name = json_string(*data, {"name"}, theme.id);
    theme.font_size = json_float(*data, {"font_size"}, theme.font_size);
    theme.radius = json_float(*data, {"corner_radius"}, theme.radius);

    theme.bg = hex_color(json_string(*data, {"vars", "surface.0"},
                     json_string(*data, {"colors", "bg"}, "")), theme.bg);
    theme.panel = hex_color(json_string(*data, {"vars", "surface.2"},
                        json_string(*data, {"colors", "panel"}, "")), theme.panel);
    theme.panel_raised = hex_color(json_string(*data, {"vars", "surface.raised"},
                               json_string(*data, {"vars", "surface.3"}, "")), theme.panel_raised);
    theme.fg = hex_color(json_string(*data, {"vars", "text.primary"},
                     json_string(*data, {"colors", "fg"}, "")), theme.fg);
    theme.muted = hex_color(json_string(*data, {"vars", "text.secondary"},
                        json_string(*data, {"vars", "text.muted"}, "")), theme.muted);
    theme.accent = hex_color(json_string(*data, {"vars", "accent.primary"},
                         json_string(*data, {"colors", "accent"}, "")), theme.accent);
    theme.accent_2 = hex_color(json_string(*data, {"vars", "accent.secondary"}, ""), theme.accent_2);
    theme.border = hex_color(json_string(*data, {"vars", "border.color"},
                         json_string(*data, {"vars", "border.default"}, "")), theme.border);
    theme.success = hex_color(json_string(*data, {"vars", "success"},
                          json_string(*data, {"vars", "accent.success"}, "")), theme.success);
    theme.warning = hex_color(json_string(*data, {"vars", "warning"},
                          json_string(*data, {"vars", "accent.warning"}, "")), theme.warning);
    theme.error = hex_color(json_string(*data, {"vars", "error"},
                        json_string(*data, {"vars", "accent.error"}, "")), theme.error);
    return theme;
}

inline std::vector<ThemeSpec> available_themes() {
    std::vector<ThemeSpec> themes;
    std::filesystem::path dir = "/etc/straylight/themes";
    try {
        if (std::filesystem::is_directory(dir)) {
            for (const auto& entry : std::filesystem::directory_iterator(dir)) {
                if (entry.is_regular_file() && entry.path().extension() == ".json") {
                    themes.push_back(load_theme(entry.path()));
                }
            }
        }
    } catch (...) {}
    std::sort(themes.begin(), themes.end(), [](const ThemeSpec& a, const ThemeSpec& b) {
        return a.name < b.name;
    });
    if (themes.empty()) themes.push_back(load_theme(theme_path("straylight-nexus")));
    return themes;
}

inline ThemeSpec active_theme() {
    return load_theme(theme_path(configured_theme_id()));
}

inline void apply_theme(const ThemeSpec& t) {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = std::min(t.radius, 10.0f);
    s.ChildRounding = std::min(t.radius, 10.0f);
    s.FrameRounding = std::min(t.radius * 0.65f, 8.0f);
    s.PopupRounding = std::min(t.radius, 10.0f);
    s.ScrollbarRounding = 8.0f;
    s.GrabRounding = std::min(t.radius * 0.65f, 8.0f);
    s.TabRounding = std::min(t.radius * 0.65f, 8.0f);
    s.WindowPadding = ImVec2(14.0f, 12.0f);
    s.FramePadding = ImVec2(8.0f, 5.0f);
    s.ItemSpacing = ImVec2(9.0f, 7.0f);
    s.ScrollbarSize = 12.0f;
    s.Colors[ImGuiCol_Text] = t.fg;
    s.Colors[ImGuiCol_TextDisabled] = with_alpha(t.muted, 0.75f);
    s.Colors[ImGuiCol_WindowBg] = with_alpha(t.bg, 0.98f);
    s.Colors[ImGuiCol_ChildBg] = with_alpha(t.panel, 0.94f);
    s.Colors[ImGuiCol_PopupBg] = with_alpha(t.panel_raised, 0.98f);
    s.Colors[ImGuiCol_Border] = with_alpha(t.border, 0.95f);
    s.Colors[ImGuiCol_FrameBg] = with_alpha(t.panel_raised, 0.78f);
    s.Colors[ImGuiCol_FrameBgHovered] = with_alpha(t.accent, 0.20f);
    s.Colors[ImGuiCol_FrameBgActive] = with_alpha(t.accent, 0.32f);
    s.Colors[ImGuiCol_TitleBg] = t.bg;
    s.Colors[ImGuiCol_TitleBgActive] = t.panel;
    s.Colors[ImGuiCol_ScrollbarBg] = with_alpha(t.bg, 0.20f);
    s.Colors[ImGuiCol_ScrollbarGrab] = with_alpha(t.border, 0.90f);
    s.Colors[ImGuiCol_ScrollbarGrabHovered] = with_alpha(t.accent, 0.55f);
    s.Colors[ImGuiCol_CheckMark] = t.accent;
    s.Colors[ImGuiCol_SliderGrab] = with_alpha(t.accent, 0.85f);
    s.Colors[ImGuiCol_SliderGrabActive] = t.accent;
    s.Colors[ImGuiCol_Button] = with_alpha(t.accent, 0.48f);
    s.Colors[ImGuiCol_ButtonHovered] = with_alpha(t.accent, 0.72f);
    s.Colors[ImGuiCol_ButtonActive] = t.accent;
    s.Colors[ImGuiCol_Header] = with_alpha(t.accent, 0.30f);
    s.Colors[ImGuiCol_HeaderHovered] = with_alpha(t.accent, 0.52f);
    s.Colors[ImGuiCol_HeaderActive] = with_alpha(t.accent, 0.72f);
    s.Colors[ImGuiCol_Separator] = with_alpha(t.border, 0.90f);
    s.Colors[ImGuiCol_Tab] = with_alpha(t.panel_raised, 0.86f);
    s.Colors[ImGuiCol_TabHovered] = with_alpha(t.accent, 0.62f);
    s.Colors[ImGuiCol_TabActive] = with_alpha(t.accent, 0.38f);
    s.Colors[ImGuiCol_TabUnfocused] = with_alpha(t.panel, 0.86f);
    s.Colors[ImGuiCol_TabUnfocusedActive] = with_alpha(t.accent_2, 0.28f);
    s.Colors[ImGuiCol_TableBorderStrong] = t.border;
    s.Colors[ImGuiCol_TableBorderLight] = with_alpha(t.border, 0.55f);
    s.Colors[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    s.Colors[ImGuiCol_TableRowBgAlt] = with_alpha(t.fg, 0.035f);
    s.Colors[ImGuiCol_PlotLines] = t.accent;
    s.Colors[ImGuiCol_PlotHistogram] = t.accent_2;
    s.Colors[ImGuiCol_NavHighlight] = with_alpha(t.accent, 0.85f);
}

inline ThemeSpec apply_straylight_theme() {
    ThemeSpec theme = active_theme();
    apply_theme(theme);
    return theme;
}

inline bool persist_user_theme(const std::string& id) {
    const char* home = std::getenv("HOME");
    if (!home || !*home) return false;
    std::filesystem::path path = std::filesystem::path(home) / ".config/straylight/theme.json";
    nlohmann::json data = nlohmann::json::object();
    if (auto existing = read_json(path)) data = *existing;
    data["theme"] = id;
    try {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path);
        file << data.dump(4) << '\n';
        return file.good();
    } catch (...) {
        return false;
    }
}

} // namespace straylight::ui
