// apps/terminal/config.cpp
// Terminal configuration loading from JSON files
#include "config.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>

namespace straylight::terminal {

using json = nlohmann::json;

static uint32_t parse_color(const json& j, uint32_t default_val) {
    if (j.is_string()) {
        std::string s = j.get<std::string>();
        // Parse "#RRGGBB" or "#AARRGGBB"
        if (s.size() >= 7 && s[0] == '#') {
            unsigned long val = 0;
            try {
                val = std::stoul(s.substr(1), nullptr, 16);
            } catch (...) {
                return default_val;
            }
            if (s.size() == 7) {
                // #RRGGBB -> add full alpha
                return 0xFF000000u | static_cast<uint32_t>(val);
            } else if (s.size() == 9) {
                return static_cast<uint32_t>(val);
            }
        }
    } else if (j.is_number_unsigned()) {
        return j.get<uint32_t>();
    }
    return default_val;
}

TerminalConfig TerminalConfig::defaults() {
    return TerminalConfig{};
}

Result<TerminalConfig, std::string>
TerminalConfig::load(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return Result<TerminalConfig, std::string>::error(
            "Failed to open config file: " + path.string());
    }

    json j;
    try {
        file >> j;
    } catch (const json::parse_error& e) {
        return Result<TerminalConfig, std::string>::error(
            std::string("JSON parse error: ") + e.what());
    }

    TerminalConfig cfg;

    if (j.contains("font_size") && j["font_size"].is_number()) {
        cfg.font_size = j["font_size"].get<float>();
    }
    if (j.contains("font_family") && j["font_family"].is_string()) {
        cfg.font_family = j["font_family"].get<std::string>();
    }
    if (j.contains("scrollback_lines") && j["scrollback_lines"].is_number_integer()) {
        cfg.scrollback_lines = j["scrollback_lines"].get<int>();
    }
    if (j.contains("shell") && j["shell"].is_string()) {
        cfg.shell = j["shell"].get<std::string>();
    }
    if (j.contains("initial_cols") && j["initial_cols"].is_number_integer()) {
        cfg.initial_cols = j["initial_cols"].get<int>();
    }
    if (j.contains("initial_rows") && j["initial_rows"].is_number_integer()) {
        cfg.initial_rows = j["initial_rows"].get<int>();
    }
    if (j.contains("cursor_blink") && j["cursor_blink"].is_boolean()) {
        cfg.cursor_blink = j["cursor_blink"].get<bool>();
    }
    if (j.contains("opacity") && j["opacity"].is_number()) {
        cfg.opacity = j["opacity"].get<float>();
    }
    if (j.contains("cursor_style") && j["cursor_style"].is_string()) {
        cfg.cursor_style = j["cursor_style"].get<std::string>();
    }

    // Color scheme
    if (j.contains("color_scheme") && j["color_scheme"].is_object()) {
        auto& cs = j["color_scheme"];
        if (cs.contains("name") && cs["name"].is_string()) {
            cfg.color_scheme.name = cs["name"].get<std::string>();
        }
        cfg.color_scheme.foreground =
            cs.contains("foreground") ? parse_color(cs["foreground"], cfg.color_scheme.foreground) : cfg.color_scheme.foreground;
        cfg.color_scheme.background =
            cs.contains("background") ? parse_color(cs["background"], cfg.color_scheme.background) : cfg.color_scheme.background;
        cfg.color_scheme.cursor =
            cs.contains("cursor") ? parse_color(cs["cursor"], cfg.color_scheme.cursor) : cfg.color_scheme.cursor;
        cfg.color_scheme.selection =
            cs.contains("selection") ? parse_color(cs["selection"], cfg.color_scheme.selection) : cfg.color_scheme.selection;

        if (cs.contains("palette") && cs["palette"].is_array()) {
            auto& pal = cs["palette"];
            for (size_t i = 0; i < std::min(pal.size(), size_t(16)); ++i) {
                cfg.color_scheme.palette[i] =
                    parse_color(pal[i], cfg.color_scheme.palette[i]);
            }
        }
    }

    return Result<TerminalConfig, std::string>::ok(std::move(cfg));
}

TerminalConfig TerminalConfig::load_or_defaults() {
    // Try standard config paths
    std::vector<std::filesystem::path> paths;

    // XDG_CONFIG_HOME/straylight/terminal.json
    const char* xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0] != '\0') {
        paths.emplace_back(std::string(xdg) + "/straylight/terminal.json");
    }

    // ~/.config/straylight/terminal.json
    const char* home = getenv("HOME");
    if (home && home[0] != '\0') {
        paths.emplace_back(std::string(home) + "/.config/straylight/terminal.json");
    }

    // /etc/straylight/terminal.json
    paths.emplace_back("/etc/straylight/terminal.json");

    for (const auto& path : paths) {
        if (std::filesystem::exists(path)) {
            auto result = load(path);
            if (result.has_value()) {
                return result.value();
            }
        }
    }

    return defaults();
}

} // namespace straylight::terminal
