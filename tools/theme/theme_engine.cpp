// tools/theme/theme_engine.cpp
#include "theme_engine.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace straylight {

namespace fs = std::filesystem;

ThemeEngine::ThemeEngine(const fs::path& themes_dir, const fs::path& user_dir)
    : system_dir_(themes_dir) {
    if (user_dir.empty()) {
        const char* home = std::getenv("HOME");
        if (home) {
            user_dir_ = fs::path(home) / ".local" / "share" / "straylight" / "themes";
        } else {
            user_dir_ = "/tmp/straylight-themes";
        }
    } else {
        user_dir_ = user_dir;
    }

    const char* home = std::getenv("HOME");
    if (home) {
        state_path_ = fs::path(home) / ".config" / "straylight" / "theme-state.json";
    } else {
        state_path_ = "/etc/straylight/theme-state.json";
    }
}

std::vector<std::string> ThemeEngine::list() const {
    std::vector<std::string> themes;

    auto scan = [&](const fs::path& dir) {
        if (!fs::exists(dir)) return;
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (entry.is_directory()) {
                fs::path theme_json = entry.path() / "theme.json";
                if (fs::exists(theme_json)) {
                    themes.push_back(entry.path().filename().string());
                }
            } else if (entry.is_regular_file() && entry.path().extension() == ".json") {
                themes.push_back(entry.path().stem().string());
            }
        }
    };

    scan(system_dir_);
    scan(user_dir_);

    std::sort(themes.begin(), themes.end());
    themes.erase(std::unique(themes.begin(), themes.end()), themes.end());
    return themes;
}

Result<Theme, std::string> ThemeEngine::load(const std::string& name) const {
    // Search user dir first, then system
    std::vector<fs::path> search_paths = {
        user_dir_ / name / "theme.json",
        user_dir_ / (name + ".json"),
        system_dir_ / name / "theme.json",
        system_dir_ / (name + ".json"),
    };

    fs::path found;
    for (const auto& p : search_paths) {
        if (fs::exists(p)) { found = p; break; }
    }

    if (found.empty()) {
        return Result<Theme, std::string>::error("Theme not found: " + name);
    }

    std::ifstream ifs(found);
    if (!ifs) {
        return Result<Theme, std::string>::error("Cannot read theme: " + found.string());
    }

    try {
        nlohmann::json j;
        ifs >> j;

        Theme theme;
        theme.name = j.value("name", name);
        theme.description = j.value("description", "");
        theme.author = j.value("author", "");
        theme.base = j.value("base", "");
        theme.gtk_theme = j.value("gtk_theme", "Adwaita");
        theme.icon_theme = j.value("icon_theme", "Papirus");
        theme.cursor_theme = j.value("cursor_theme", "default");
        theme.cursor_size = j.value("cursor_size", 24);
        theme.font_family = j.value("font_family", "Inter");
        theme.font_size = j.value("font_size", 11);
        theme.wallpaper = j.value("wallpaper", "");
        theme.border_radius = j.value("border_radius", 8.0f);
        theme.opacity = j.value("opacity", 1.0f);

        if (j.contains("colors") && j["colors"].is_object()) {
            auto& c = j["colors"];
            theme.colors.background = c.value("background", "#1a1a2e");
            theme.colors.foreground = c.value("foreground", "#e0e0e0");
            theme.colors.primary = c.value("primary", "#00d4ff");
            theme.colors.secondary = c.value("secondary", "#7b2ff7");
            theme.colors.accent = c.value("accent", "#ff6b6b");
            theme.colors.error = c.value("error", "#f44336");
            theme.colors.warning = c.value("warning", "#ff9800");
            theme.colors.success = c.value("success", "#4caf50");
            theme.colors.surface = c.value("surface", "#16213e");
            theme.colors.border = c.value("border", "#2a2a4a");

            for (auto& [k, v] : c.items()) {
                if (k != "background" && k != "foreground" && k != "primary" &&
                    k != "secondary" && k != "accent" && k != "error" &&
                    k != "warning" && k != "success" && k != "surface" && k != "border") {
                    theme.colors.extra[k] = v.get<std::string>();
                }
            }
        }

        // Resolve inheritance
        if (!theme.base.empty()) {
            theme = resolve_inheritance(theme);
        }

        return Result<Theme, std::string>::ok(std::move(theme));
    } catch (const std::exception& e) {
        return Result<Theme, std::string>::error(
            std::string("Parse error in theme '") + name + "': " + e.what());
    }
}

Theme ThemeEngine::resolve_inheritance(const Theme& theme) const {
    if (theme.base.empty()) return theme;

    auto base_res = load(theme.base);
    if (!base_res.has_value()) return theme;

    Theme merged = base_res.value();
    merged.name = theme.name;
    merged.description = theme.description;
    merged.author = theme.author;
    merged.base = theme.base;

    // Override non-empty values from child
    if (!theme.colors.background.empty()) merged.colors.background = theme.colors.background;
    if (!theme.colors.foreground.empty()) merged.colors.foreground = theme.colors.foreground;
    if (!theme.colors.primary.empty()) merged.colors.primary = theme.colors.primary;
    if (!theme.colors.secondary.empty()) merged.colors.secondary = theme.colors.secondary;
    if (!theme.colors.accent.empty()) merged.colors.accent = theme.colors.accent;
    if (!theme.colors.error.empty()) merged.colors.error = theme.colors.error;
    if (!theme.colors.warning.empty()) merged.colors.warning = theme.colors.warning;
    if (!theme.colors.success.empty()) merged.colors.success = theme.colors.success;
    if (!theme.colors.surface.empty()) merged.colors.surface = theme.colors.surface;
    if (!theme.colors.border.empty()) merged.colors.border = theme.colors.border;
    for (const auto& [k, v] : theme.colors.extra) {
        merged.colors.extra[k] = v;
    }

    if (!theme.gtk_theme.empty()) merged.gtk_theme = theme.gtk_theme;
    if (!theme.icon_theme.empty()) merged.icon_theme = theme.icon_theme;
    if (!theme.cursor_theme.empty()) merged.cursor_theme = theme.cursor_theme;
    if (!theme.font_family.empty()) merged.font_family = theme.font_family;
    if (!theme.wallpaper.empty()) merged.wallpaper = theme.wallpaper;
    if (theme.cursor_size != 24) merged.cursor_size = theme.cursor_size;
    if (theme.font_size != 11) merged.font_size = theme.font_size;

    return merged;
}

Result<void, std::string> ThemeEngine::apply(const std::string& name) {
    auto theme_res = load(name);
    if (!theme_res.has_value()) {
        return Result<void, std::string>::error(theme_res.error());
    }

    const auto& theme = theme_res.value();

    auto comp_res = apply_compositor(theme);
    if (!comp_res.has_value()) {
        return Result<void, std::string>::error("Compositor: " + comp_res.error());
    }

    auto gtk_res = apply_gtk(theme);
    if (!gtk_res.has_value()) {
        return Result<void, std::string>::error("GTK: " + gtk_res.error());
    }

    auto icon_res = apply_icons(theme);
    if (!icon_res.has_value()) {
        return Result<void, std::string>::error("Icons: " + icon_res.error());
    }

    // Save active theme state
    std::error_code ec;
    fs::create_directories(state_path_.parent_path(), ec);
    nlohmann::json state;
    state["active_theme"] = name;
    std::ofstream sofs(state_path_);
    if (sofs) sofs << state.dump(2) << "\n";

    return Result<void, std::string>::ok();
}

Result<void, std::string> ThemeEngine::create(const std::string& name,
                                                const std::string& base) {
    fs::path theme_dir = user_dir_ / name;
    std::error_code ec;
    fs::create_directories(theme_dir, ec);
    if (ec) {
        return Result<void, std::string>::error("Cannot create directory: " + ec.message());
    }

    Theme theme;
    theme.name = name;
    theme.base = (base == "default" || base.empty()) ? "" : base;
    theme.description = "Custom StrayLight theme";
    theme.author = std::getenv("USER") ? std::getenv("USER") : "unknown";

    if (!base.empty() && base != "default") {
        auto base_res = load(base);
        if (base_res.has_value()) {
            theme = base_res.value();
            theme.name = name;
            theme.base = base;
        }
    } else {
        // StrayLight default palette
        theme.colors.background = "#1a1a2e";
        theme.colors.foreground = "#e0e0e0";
        theme.colors.primary = "#00d4ff";
        theme.colors.secondary = "#7b2ff7";
        theme.colors.accent = "#ff6b6b";
        theme.colors.error = "#f44336";
        theme.colors.warning = "#ff9800";
        theme.colors.success = "#4caf50";
        theme.colors.surface = "#16213e";
        theme.colors.border = "#2a2a4a";
    }

    return save_theme(theme);
}

Result<void, std::string> ThemeEngine::save_theme(const Theme& theme) const {
    fs::path theme_dir = user_dir_ / theme.name;
    std::error_code ec;
    fs::create_directories(theme_dir, ec);

    nlohmann::json j;
    j["name"] = theme.name;
    j["description"] = theme.description;
    j["author"] = theme.author;
    if (!theme.base.empty()) j["base"] = theme.base;
    j["gtk_theme"] = theme.gtk_theme;
    j["icon_theme"] = theme.icon_theme;
    j["cursor_theme"] = theme.cursor_theme;
    j["cursor_size"] = theme.cursor_size;
    j["font_family"] = theme.font_family;
    j["font_size"] = theme.font_size;
    if (!theme.wallpaper.empty()) j["wallpaper"] = theme.wallpaper;
    j["border_radius"] = theme.border_radius;
    j["opacity"] = theme.opacity;

    nlohmann::json colors;
    colors["background"] = theme.colors.background;
    colors["foreground"] = theme.colors.foreground;
    colors["primary"] = theme.colors.primary;
    colors["secondary"] = theme.colors.secondary;
    colors["accent"] = theme.colors.accent;
    colors["error"] = theme.colors.error;
    colors["warning"] = theme.colors.warning;
    colors["success"] = theme.colors.success;
    colors["surface"] = theme.colors.surface;
    colors["border"] = theme.colors.border;
    for (const auto& [k, v] : theme.colors.extra) {
        colors[k] = v;
    }
    j["colors"] = colors;

    fs::path path = theme_dir / "theme.json";
    std::ofstream ofs(path);
    if (!ofs) {
        return Result<void, std::string>::error("Cannot write: " + path.string());
    }
    ofs << j.dump(2) << "\n";
    return Result<void, std::string>::ok();
}

std::string ThemeEngine::preview(const std::string& name) const {
    auto theme_res = load(name);
    if (!theme_res.has_value()) return "Error: " + theme_res.error();

    const auto& theme = theme_res.value();
    std::ostringstream out;

    out << "\033[1m" << theme.name << "\033[0m";
    if (!theme.description.empty()) out << " — " << theme.description;
    if (!theme.author.empty()) out << " by " << theme.author;
    out << "\n\n";

    // Color swatches using ANSI true color
    auto swatch = [&](const std::string& label, const std::string& hex) {
        auto rgb = hex_to_rgb(hex);
        out << "  " << ansi_bg(rgb) << "    \033[0m "
            << std::left;
        // Pad label to 12 chars
        std::string padded = label;
        while (padded.size() < 12) padded += ' ';
        out << padded << " " << hex << "\n";
    };

    swatch("Background", theme.colors.background);
    swatch("Foreground", theme.colors.foreground);
    swatch("Primary",    theme.colors.primary);
    swatch("Secondary",  theme.colors.secondary);
    swatch("Accent",     theme.colors.accent);
    swatch("Error",      theme.colors.error);
    swatch("Warning",    theme.colors.warning);
    swatch("Success",    theme.colors.success);
    swatch("Surface",    theme.colors.surface);
    swatch("Border",     theme.colors.border);

    for (const auto& [k, v] : theme.colors.extra) {
        swatch(k, v);
    }

    out << "\n  GTK: " << theme.gtk_theme
        << "  Icons: " << theme.icon_theme
        << "  Cursor: " << theme.cursor_theme
        << "  Font: " << theme.font_family << " " << theme.font_size << "pt\n";

    return out.str();
}

Result<std::string, std::string> ThemeEngine::export_json(const std::string& name) const {
    auto theme_res = load(name);
    if (!theme_res.has_value()) {
        return Result<std::string, std::string>::error(theme_res.error());
    }

    // Re-serialize (save_theme format but to string)
    const auto& theme = theme_res.value();
    nlohmann::json j;
    j["name"] = theme.name;
    j["description"] = theme.description;
    j["author"] = theme.author;
    j["colors"] = {
        {"background", theme.colors.background},
        {"foreground", theme.colors.foreground},
        {"primary", theme.colors.primary},
        {"secondary", theme.colors.secondary},
        {"accent", theme.colors.accent},
        {"error", theme.colors.error},
        {"warning", theme.colors.warning},
        {"success", theme.colors.success},
        {"surface", theme.colors.surface},
        {"border", theme.colors.border},
    };
    for (const auto& [k, v] : theme.colors.extra) {
        j["colors"][k] = v;
    }
    j["gtk_theme"] = theme.gtk_theme;
    j["icon_theme"] = theme.icon_theme;
    j["cursor_theme"] = theme.cursor_theme;
    j["font_family"] = theme.font_family;
    j["font_size"] = theme.font_size;

    return Result<std::string, std::string>::ok(j.dump(2));
}

Result<void, std::string> ThemeEngine::import_json(const std::string& json_str) {
    try {
        auto j = nlohmann::json::parse(json_str);
        Theme theme;
        theme.name = j.value("name", "imported");
        theme.description = j.value("description", "Imported theme");
        theme.author = j.value("author", "");

        if (j.contains("colors") && j["colors"].is_object()) {
            auto& c = j["colors"];
            theme.colors.background = c.value("background", "#1a1a2e");
            theme.colors.foreground = c.value("foreground", "#e0e0e0");
            theme.colors.primary = c.value("primary", "#00d4ff");
            theme.colors.secondary = c.value("secondary", "#7b2ff7");
            theme.colors.accent = c.value("accent", "#ff6b6b");
            theme.colors.error = c.value("error", "#f44336");
            theme.colors.warning = c.value("warning", "#ff9800");
            theme.colors.success = c.value("success", "#4caf50");
            theme.colors.surface = c.value("surface", "#16213e");
            theme.colors.border = c.value("border", "#2a2a4a");
        }

        theme.gtk_theme = j.value("gtk_theme", "Adwaita");
        theme.icon_theme = j.value("icon_theme", "Papirus");
        theme.cursor_theme = j.value("cursor_theme", "default");
        theme.font_family = j.value("font_family", "Inter");
        theme.font_size = j.value("font_size", 11);

        return save_theme(theme);
    } catch (const std::exception& e) {
        return Result<void, std::string>::error(
            std::string("Parse error: ") + e.what());
    }
}

std::string ThemeEngine::active_theme() const {
    if (fs::exists(state_path_)) {
        std::ifstream ifs(state_path_);
        if (ifs) {
            try {
                nlohmann::json j;
                ifs >> j;
                return j.value("active_theme", "default");
            } catch (...) {}
        }
    }
    return "default";
}

Result<void, std::string> ThemeEngine::apply_compositor(const Theme& theme) const {
    fs::path config = "/etc/straylight/compositor.d/theme.conf";
    std::error_code ec;
    fs::create_directories(config.parent_path(), ec);

    std::ofstream ofs(config);
    if (!ofs) {
        return Result<void, std::string>::error("Cannot write: " + config.string());
    }

    ofs << "# StrayLight compositor theme\n"
        << "# Auto-generated by straylight-theme\n\n"
        << "[theme]\n"
        << "background = " << theme.colors.background << "\n"
        << "foreground = " << theme.colors.foreground << "\n"
        << "primary = " << theme.colors.primary << "\n"
        << "secondary = " << theme.colors.secondary << "\n"
        << "accent = " << theme.colors.accent << "\n"
        << "surface = " << theme.colors.surface << "\n"
        << "border = " << theme.colors.border << "\n"
        << "border_radius = " << theme.border_radius << "\n"
        << "opacity = " << theme.opacity << "\n"
        << "font_family = " << theme.font_family << "\n"
        << "font_size = " << theme.font_size << "\n";

    if (!theme.wallpaper.empty()) {
        ofs << "wallpaper = " << theme.wallpaper << "\n";
    }

    return Result<void, std::string>::ok();
}

Result<void, std::string> ThemeEngine::apply_gtk(const Theme& theme) const {
    const char* home = std::getenv("HOME");
    if (!home) return Result<void, std::string>::ok();

    fs::path gtk3_settings = fs::path(home) / ".config" / "gtk-3.0" / "settings.ini";
    fs::path gtk4_settings = fs::path(home) / ".config" / "gtk-4.0" / "settings.ini";

    auto write_gtk = [&](const fs::path& path) {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        std::ofstream ofs(path);
        if (!ofs) return;
        ofs << "[Settings]\n"
            << "gtk-theme-name=" << theme.gtk_theme << "\n"
            << "gtk-icon-theme-name=" << theme.icon_theme << "\n"
            << "gtk-cursor-theme-name=" << theme.cursor_theme << "\n"
            << "gtk-cursor-theme-size=" << theme.cursor_size << "\n"
            << "gtk-font-name=" << theme.font_family << " " << theme.font_size << "\n";
    };

    write_gtk(gtk3_settings);
    write_gtk(gtk4_settings);

    return Result<void, std::string>::ok();
}

Result<void, std::string> ThemeEngine::apply_icons(const Theme& theme) const {
    const char* home = std::getenv("HOME");
    if (!home) return Result<void, std::string>::ok();

    fs::path icon_dir = fs::path(home) / ".icons" / "default";
    std::error_code ec;
    fs::create_directories(icon_dir, ec);

    fs::path index = icon_dir / "index.theme";
    std::ofstream ofs(index);
    if (ofs) {
        ofs << "[Icon Theme]\n"
            << "Name=Default\n"
            << "Comment=Default Cursor Theme\n"
            << "Inherits=" << theme.cursor_theme << "\n";
    }

    return Result<void, std::string>::ok();
}

ThemeEngine::RGB ThemeEngine::hex_to_rgb(const std::string& hex) {
    RGB rgb{0, 0, 0};
    std::string h = hex;
    if (!h.empty() && h[0] == '#') h = h.substr(1);
    if (h.size() >= 6) {
        rgb.r = std::stoi(h.substr(0, 2), nullptr, 16);
        rgb.g = std::stoi(h.substr(2, 2), nullptr, 16);
        rgb.b = std::stoi(h.substr(4, 2), nullptr, 16);
    }
    return rgb;
}

std::string ThemeEngine::ansi_bg(const RGB& color) {
    return "\033[48;2;" + std::to_string(color.r) + ";" +
           std::to_string(color.g) + ";" + std::to_string(color.b) + "m";
}

std::string ThemeEngine::ansi_fg(const RGB& color) {
    return "\033[38;2;" + std::to_string(color.r) + ";" +
           std::to_string(color.g) + ";" + std::to_string(color.b) + "m";
}

} // namespace straylight
