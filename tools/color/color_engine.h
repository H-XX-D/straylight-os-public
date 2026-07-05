// tools/color/color_engine.h
#pragma once

#include <straylight/result.h>
#include <straylight/error.h>

#include <cstdint>
#include <string>
#include <vector>

namespace straylight {

/// An RGBA color in floating-point [0.0, 1.0] per channel.
struct Color {
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    double a = 1.0;
};

/// HSL color representation.
struct HSL { double h, s, l, a; };

/// HSV color representation.
struct HSV { double h, s, v, a; };

/// CMYK color representation.
struct CMYK { double c, m, y, k; };

/// CIE Lab color representation.
struct Lab { double L, a_star, b_star; };

/// OKLCh color representation.
struct OKLCh { double L, C, h; };

/// A named color in a palette.
struct PaletteColor {
    std::string name;
    Color color;
};

/// A saved color palette.
struct Palette {
    std::string name;
    std::string description;
    std::vector<PaletteColor> colors;
};

/// Color scheme generation types.
enum class SchemeType {
    Complementary,
    Analogous,
    Triadic,
    SplitComplementary,
    Tetradic,
    Monochromatic,
};

/// Parse a SchemeType from a string name.
Result<SchemeType, SLError> parse_scheme_type(const std::string& name);

/// Color engine: conversion, palette management, scheme generation, export.
class ColorEngine {
public:
    // -----------------------------------------------------------------------
    // Color space conversions
    // -----------------------------------------------------------------------

    static Color from_hex(const std::string& hex);
    static std::string to_hex(const Color& c);

    static HSL to_hsl(const Color& c);
    static Color from_hsl(const HSL& hsl);

    static HSV to_hsv(const Color& c);
    static Color from_hsv(const HSV& hsv);

    static CMYK to_cmyk(const Color& c);
    static Color from_cmyk(const CMYK& cmyk);

    static Lab to_lab(const Color& c);
    static Color from_lab(const Lab& lab);

    static OKLCh to_oklch(const Color& c);
    static Color from_oklch(const OKLCh& oklch);

    /// Parse a color from any supported format string.
    static Result<Color, SLError> parse(const std::string& str);

    /// Format a color to a given color space name string.
    static std::string format(const Color& c, const std::string& space);

    /// Convert a color string from one space to another.
    static Result<std::string, SLError> convert(const std::string& color_str,
                                                const std::string& from_space,
                                                const std::string& to_space);

    // -----------------------------------------------------------------------
    // Color scheme generation
    // -----------------------------------------------------------------------

    static std::vector<Color> generate_scheme(const Color& base, SchemeType type);

    // -----------------------------------------------------------------------
    // Screen color picker
    // -----------------------------------------------------------------------

    /// Pick a color from the screen via compositor screenshot + pixel sampling.
    Result<Color, SLError> pick_screen_color() const;

    // -----------------------------------------------------------------------
    // Palette management
    // -----------------------------------------------------------------------

    Result<void, SLError> create_palette(const Palette& palette);
    Result<std::vector<std::string>, SLError> list_palettes() const;
    Result<Palette, SLError> load_palette(const std::string& name) const;
    Result<void, SLError> delete_palette(const std::string& name);

    // -----------------------------------------------------------------------
    // Export
    // -----------------------------------------------------------------------

    static std::string export_css(const Palette& palette);
    static std::string export_scss(const Palette& palette);
    static std::string export_json(const Palette& palette);
    static std::string export_svg(const Palette& palette);

    /// Export a palette in the specified format.
    static Result<std::string, SLError> export_palette(const Palette& palette,
                                                       const std::string& format);

    // -----------------------------------------------------------------------
    // Theme integration
    // -----------------------------------------------------------------------

    /// Apply a palette to the StrayLight theme system.
    Result<void, SLError> apply_theme(const Palette& palette) const;

private:
    /// Get the palette storage directory.
    static std::filesystem::path palette_dir();

    /// Palette file path for a given name.
    static std::filesystem::path palette_path(const std::string& name);

    // Internal: linear RGB <-> sRGB gamma.
    static double srgb_to_linear(double c);
    static double linear_to_srgb(double c);

    // Internal: XYZ conversions.
    struct XYZ { double x, y, z; };
    static XYZ rgb_to_xyz(const Color& c);
    static Color xyz_to_rgb(const XYZ& xyz);
};

} // namespace straylight
