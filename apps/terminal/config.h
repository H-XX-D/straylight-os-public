// apps/terminal/config.h
// Terminal emulator configuration loaded from JSON
#pragma once

#include <straylight/result.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace straylight::terminal {

/// Color scheme definition.
struct ColorScheme {
    std::string name = "cyberpunk";
    uint32_t foreground = 0xFFCCCCCC;
    uint32_t background = 0xFF1A1A2E;
    uint32_t cursor = 0xFF00FFAA;
    uint32_t selection = 0xFF3A3A5E;
    std::array<uint32_t, 16> palette = {{
        0xFF1A1A2E, 0xFFE74C3C, 0xFF2ECC71, 0xFFF39C12,
        0xFF3498DB, 0xFF9B59B6, 0xFF1ABC9C, 0xFFCCCCCC,
        0xFF555555, 0xFFFF6B6B, 0xFF55EFC4, 0xFFFFD93D,
        0xFF74B9FF, 0xFFDDA0DD, 0xFF48DBFB, 0xFFFFFFFF,
    }};
};

/// Terminal configuration.
struct TerminalConfig {
    float font_size = 14.0f;
    std::string font_family = "monospace";
    int scrollback_lines = 10000;
    ColorScheme color_scheme;
    std::string shell; // empty = auto-detect
    int initial_cols = 80;
    int initial_rows = 24;
    bool cursor_blink = true;
    float opacity = 0.95f;
    std::string cursor_style = "block"; // block, underline, bar

    /// Load config from a JSON file.
    static Result<TerminalConfig, std::string>
    load(const std::filesystem::path& path);

    /// Load default config.
    static TerminalConfig defaults();

    /// Try to load from standard config paths, fall back to defaults.
    static TerminalConfig load_or_defaults();
};

} // namespace straylight::terminal
