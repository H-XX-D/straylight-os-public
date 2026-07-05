// tools/theme/theme_engine.h
// Theme engine — manage and create StrayLight visual themes.
#pragma once

#include <straylight/result.h>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace straylight {

/// Color palette for a theme.
struct ThemeColors {
    std::string background;
    std::string foreground;
    std::string primary;
    std::string secondary;
    std::string accent;
    std::string error;
    std::string warning;
    std::string success;
    std::string surface;
    std::string border;
    std::map<std::string, std::string> extra;
};

/// A complete theme definition.
struct Theme {
    std::string name;
    std::string description;
    std::string author;
    std::string base;
    ThemeColors colors;
    std::string gtk_theme;
    std::string icon_theme;
    std::string cursor_theme;
    int cursor_size = 24;
    std::string font_family;
    int font_size = 11;
    std::string wallpaper;
    float border_radius = 8.0f;
    float opacity = 1.0f;
};

class ThemeEngine {
public:
    explicit ThemeEngine(const std::filesystem::path& themes_dir = "/usr/share/straylight/themes",
                          const std::filesystem::path& user_dir = "");

    std::vector<std::string> list() const;
    Result<Theme, std::string> load(const std::string& name) const;
    Result<void, std::string> apply(const std::string& name);
    Result<void, std::string> create(const std::string& name, const std::string& base = "default");
    std::string preview(const std::string& name) const;
    Result<void, std::string> save_theme(const Theme& theme) const;
    Result<std::string, std::string> export_json(const std::string& name) const;
    Result<void, std::string> import_json(const std::string& json_str);
    std::string active_theme() const;

private:
    Theme resolve_inheritance(const Theme& theme) const;
    Result<void, std::string> apply_compositor(const Theme& theme) const;
    Result<void, std::string> apply_gtk(const Theme& theme) const;
    Result<void, std::string> apply_icons(const Theme& theme) const;

    struct RGB { int r, g, b; };
    static RGB hex_to_rgb(const std::string& hex);
    static std::string ansi_bg(const RGB& color);
    static std::string ansi_fg(const RGB& color);

    std::filesystem::path system_dir_;
    std::filesystem::path user_dir_;
    std::filesystem::path state_path_;
};

} // namespace straylight
