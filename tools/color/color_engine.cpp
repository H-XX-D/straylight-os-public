// tools/color/color_engine.cpp
#include "color_engine.h"

#include <straylight/log.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace fs = std::filesystem;

namespace straylight {

// ---------------------------------------------------------------------------
// SchemeType parsing
// ---------------------------------------------------------------------------

Result<SchemeType, SLError> parse_scheme_type(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower == "complementary")       return Result<SchemeType, SLError>::ok(SchemeType::Complementary);
    if (lower == "analogous")           return Result<SchemeType, SLError>::ok(SchemeType::Analogous);
    if (lower == "triadic")             return Result<SchemeType, SLError>::ok(SchemeType::Triadic);
    if (lower == "split-complementary" || lower == "split_complementary" || lower == "splitcomplementary")
        return Result<SchemeType, SLError>::ok(SchemeType::SplitComplementary);
    if (lower == "tetradic")            return Result<SchemeType, SLError>::ok(SchemeType::Tetradic);
    if (lower == "monochromatic")       return Result<SchemeType, SLError>::ok(SchemeType::Monochromatic);

    return Result<SchemeType, SLError>::error(
        {SLErrorCode::InvalidArgument,
         "Unknown scheme type: " + name +
         " (valid: complementary, analogous, triadic, split-complementary, tetradic, monochromatic)"});
}

// ---------------------------------------------------------------------------
// Gamma helpers
// ---------------------------------------------------------------------------

double ColorEngine::srgb_to_linear(double c) {
    if (c <= 0.04045) return c / 12.92;
    return std::pow((c + 0.055) / 1.055, 2.4);
}

double ColorEngine::linear_to_srgb(double c) {
    if (c <= 0.0031308) return c * 12.92;
    return 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
}

// ---------------------------------------------------------------------------
// XYZ conversions (D65 illuminant)
// ---------------------------------------------------------------------------

ColorEngine::XYZ ColorEngine::rgb_to_xyz(const Color& c) {
    double r = srgb_to_linear(c.r);
    double g = srgb_to_linear(c.g);
    double b = srgb_to_linear(c.b);

    XYZ xyz;
    xyz.x = 0.4124564 * r + 0.3575761 * g + 0.1804375 * b;
    xyz.y = 0.2126729 * r + 0.7151522 * g + 0.0721750 * b;
    xyz.z = 0.0193339 * r + 0.1191920 * g + 0.9503041 * b;
    return xyz;
}

Color ColorEngine::xyz_to_rgb(const XYZ& xyz) {
    double r =  3.2404542 * xyz.x - 1.5371385 * xyz.y - 0.4985314 * xyz.z;
    double g = -0.9692660 * xyz.x + 1.8760108 * xyz.y + 0.0415560 * xyz.z;
    double b =  0.0556434 * xyz.x - 0.2040259 * xyz.y + 1.0572252 * xyz.z;

    Color c;
    c.r = std::clamp(linear_to_srgb(r), 0.0, 1.0);
    c.g = std::clamp(linear_to_srgb(g), 0.0, 1.0);
    c.b = std::clamp(linear_to_srgb(b), 0.0, 1.0);
    c.a = 1.0;
    return c;
}

// ---------------------------------------------------------------------------
// Hex conversion
// ---------------------------------------------------------------------------

Color ColorEngine::from_hex(const std::string& hex) {
    std::string h = hex;
    if (!h.empty() && h[0] == '#') h = h.substr(1);

    // Expand shorthand (#RGB -> #RRGGBB).
    if (h.size() == 3) {
        std::string expanded;
        for (char c : h) { expanded += c; expanded += c; }
        h = expanded;
    }

    Color c;
    if (h.size() >= 6) {
        unsigned int val = 0;
        std::istringstream iss(h.substr(0, 6));
        iss >> std::hex >> val;
        c.r = ((val >> 16) & 0xFF) / 255.0;
        c.g = ((val >> 8) & 0xFF) / 255.0;
        c.b = (val & 0xFF) / 255.0;
    }
    if (h.size() == 8) {
        unsigned int alpha = 0;
        std::istringstream iss(h.substr(6, 2));
        iss >> std::hex >> alpha;
        c.a = alpha / 255.0;
    }
    return c;
}

std::string ColorEngine::to_hex(const Color& c) {
    auto clamp8 = [](double v) -> int {
        return std::clamp(static_cast<int>(std::round(v * 255.0)), 0, 255);
    };
    std::ostringstream oss;
    oss << "#" << std::hex << std::setfill('0')
        << std::setw(2) << clamp8(c.r)
        << std::setw(2) << clamp8(c.g)
        << std::setw(2) << clamp8(c.b);
    return oss.str();
}

// ---------------------------------------------------------------------------
// HSL
// ---------------------------------------------------------------------------

HSL ColorEngine::to_hsl(const Color& c) {
    double max_c = std::max({c.r, c.g, c.b});
    double min_c = std::min({c.r, c.g, c.b});
    double delta = max_c - min_c;

    HSL hsl;
    hsl.a = c.a;
    hsl.l = (max_c + min_c) / 2.0;

    if (delta < 1e-10) {
        hsl.h = 0.0;
        hsl.s = 0.0;
        return hsl;
    }

    hsl.s = (hsl.l > 0.5) ? delta / (2.0 - max_c - min_c)
                           : delta / (max_c + min_c);

    if (max_c == c.r) {
        hsl.h = std::fmod((c.g - c.b) / delta, 6.0);
    } else if (max_c == c.g) {
        hsl.h = (c.b - c.r) / delta + 2.0;
    } else {
        hsl.h = (c.r - c.g) / delta + 4.0;
    }
    hsl.h *= 60.0;
    if (hsl.h < 0.0) hsl.h += 360.0;

    return hsl;
}

Color ColorEngine::from_hsl(const HSL& hsl) {
    if (hsl.s < 1e-10) {
        return {hsl.l, hsl.l, hsl.l, hsl.a};
    }

    double c = (1.0 - std::abs(2.0 * hsl.l - 1.0)) * hsl.s;
    double x = c * (1.0 - std::abs(std::fmod(hsl.h / 60.0, 2.0) - 1.0));
    double m = hsl.l - c / 2.0;

    double r = 0, g = 0, b = 0;
    double h = hsl.h;
    if (h < 60)       { r = c; g = x; b = 0; }
    else if (h < 120) { r = x; g = c; b = 0; }
    else if (h < 180) { r = 0; g = c; b = x; }
    else if (h < 240) { r = 0; g = x; b = c; }
    else if (h < 300) { r = x; g = 0; b = c; }
    else              { r = c; g = 0; b = x; }

    return {std::clamp(r + m, 0.0, 1.0),
            std::clamp(g + m, 0.0, 1.0),
            std::clamp(b + m, 0.0, 1.0),
            hsl.a};
}

// ---------------------------------------------------------------------------
// HSV
// ---------------------------------------------------------------------------

HSV ColorEngine::to_hsv(const Color& c) {
    double max_c = std::max({c.r, c.g, c.b});
    double min_c = std::min({c.r, c.g, c.b});
    double delta = max_c - min_c;

    HSV hsv;
    hsv.a = c.a;
    hsv.v = max_c;
    hsv.s = (max_c < 1e-10) ? 0.0 : delta / max_c;

    if (delta < 1e-10) {
        hsv.h = 0.0;
        return hsv;
    }

    if (max_c == c.r) {
        hsv.h = std::fmod((c.g - c.b) / delta, 6.0);
    } else if (max_c == c.g) {
        hsv.h = (c.b - c.r) / delta + 2.0;
    } else {
        hsv.h = (c.r - c.g) / delta + 4.0;
    }
    hsv.h *= 60.0;
    if (hsv.h < 0.0) hsv.h += 360.0;

    return hsv;
}

Color ColorEngine::from_hsv(const HSV& hsv) {
    double c = hsv.v * hsv.s;
    double x = c * (1.0 - std::abs(std::fmod(hsv.h / 60.0, 2.0) - 1.0));
    double m = hsv.v - c;

    double r = 0, g = 0, b = 0;
    double h = hsv.h;
    if (h < 60)       { r = c; g = x; }
    else if (h < 120) { r = x; g = c; }
    else if (h < 180) { g = c; b = x; }
    else if (h < 240) { g = x; b = c; }
    else if (h < 300) { r = x; b = c; }
    else              { r = c; b = x; }

    return {std::clamp(r + m, 0.0, 1.0),
            std::clamp(g + m, 0.0, 1.0),
            std::clamp(b + m, 0.0, 1.0),
            hsv.a};
}

// ---------------------------------------------------------------------------
// CMYK
// ---------------------------------------------------------------------------

CMYK ColorEngine::to_cmyk(const Color& c) {
    double k = 1.0 - std::max({c.r, c.g, c.b});
    if (k >= 1.0 - 1e-10) return {0.0, 0.0, 0.0, 1.0};

    return {
        (1.0 - c.r - k) / (1.0 - k),
        (1.0 - c.g - k) / (1.0 - k),
        (1.0 - c.b - k) / (1.0 - k),
        k
    };
}

Color ColorEngine::from_cmyk(const CMYK& cmyk) {
    double r = (1.0 - cmyk.c) * (1.0 - cmyk.k);
    double g = (1.0 - cmyk.m) * (1.0 - cmyk.k);
    double b = (1.0 - cmyk.y) * (1.0 - cmyk.k);
    return {std::clamp(r, 0.0, 1.0),
            std::clamp(g, 0.0, 1.0),
            std::clamp(b, 0.0, 1.0),
            1.0};
}

// ---------------------------------------------------------------------------
// CIE Lab (D65)
// ---------------------------------------------------------------------------

Lab ColorEngine::to_lab(const Color& c) {
    XYZ xyz = rgb_to_xyz(c);

    // D65 reference white.
    constexpr double xn = 0.95047, yn = 1.00000, zn = 1.08883;

    auto f = [](double t) -> double {
        constexpr double delta = 6.0 / 29.0;
        if (t > delta * delta * delta) return std::cbrt(t);
        return t / (3.0 * delta * delta) + 4.0 / 29.0;
    };

    double fx = f(xyz.x / xn);
    double fy = f(xyz.y / yn);
    double fz = f(xyz.z / zn);

    Lab lab;
    lab.L = 116.0 * fy - 16.0;
    lab.a_star = 500.0 * (fx - fy);
    lab.b_star = 200.0 * (fy - fz);
    return lab;
}

Color ColorEngine::from_lab(const Lab& lab) {
    constexpr double xn = 0.95047, yn = 1.00000, zn = 1.08883;

    double fy = (lab.L + 16.0) / 116.0;
    double fx = lab.a_star / 500.0 + fy;
    double fz = fy - lab.b_star / 200.0;

    auto finv = [](double t) -> double {
        constexpr double delta = 6.0 / 29.0;
        if (t > delta) return t * t * t;
        return 3.0 * delta * delta * (t - 4.0 / 29.0);
    };

    XYZ xyz;
    xyz.x = xn * finv(fx);
    xyz.y = yn * finv(fy);
    xyz.z = zn * finv(fz);

    return xyz_to_rgb(xyz);
}

// ---------------------------------------------------------------------------
// OKLCh
// ---------------------------------------------------------------------------

OKLCh ColorEngine::to_oklch(const Color& c) {
    // Convert via OKLab.
    double lr = srgb_to_linear(c.r);
    double lg = srgb_to_linear(c.g);
    double lb = srgb_to_linear(c.b);

    double l_ = 0.4122214708 * lr + 0.5363325363 * lg + 0.0514459929 * lb;
    double m_ = 0.2119034982 * lr + 0.6806995451 * lg + 0.1073969566 * lb;
    double s_ = 0.0883024619 * lr + 0.2817188376 * lg + 0.6299787005 * lb;

    double l_c = std::cbrt(l_);
    double m_c = std::cbrt(m_);
    double s_c = std::cbrt(s_);

    double L = 0.2104542553 * l_c + 0.7936177850 * m_c - 0.0040720468 * s_c;
    double a = 1.9779984951 * l_c - 2.4285922050 * m_c + 0.4505937099 * s_c;
    double b = 0.0259040371 * l_c + 0.7827717662 * m_c - 0.8086757660 * s_c;

    double C = std::sqrt(a * a + b * b);
    double h = std::atan2(b, a) * 180.0 / M_PI;
    if (h < 0.0) h += 360.0;

    return {L, C, h};
}

Color ColorEngine::from_oklch(const OKLCh& oklch) {
    double a = oklch.C * std::cos(oklch.h * M_PI / 180.0);
    double b = oklch.C * std::sin(oklch.h * M_PI / 180.0);

    double l_c = oklch.L + 0.3963377774 * a + 0.2158037573 * b;
    double m_c = oklch.L - 0.1055613458 * a - 0.0638541728 * b;
    double s_c = oklch.L - 0.0894841775 * a - 1.2914855480 * b;

    double l_ = l_c * l_c * l_c;
    double m_ = m_c * m_c * m_c;
    double s_ = s_c * s_c * s_c;

    double r =  4.0767416621 * l_ - 3.3077115913 * m_ + 0.2309699292 * s_;
    double g = -1.2684380046 * l_ + 2.6097574011 * m_ - 0.3413193965 * s_;
    double bl = -0.0041960863 * l_ - 0.7034186147 * m_ + 1.7076147010 * s_;

    Color c;
    c.r = std::clamp(linear_to_srgb(r), 0.0, 1.0);
    c.g = std::clamp(linear_to_srgb(g), 0.0, 1.0);
    c.b = std::clamp(linear_to_srgb(bl), 0.0, 1.0);
    c.a = 1.0;
    return c;
}

// ---------------------------------------------------------------------------
// Parse / Format / Convert
// ---------------------------------------------------------------------------

Result<Color, SLError> ColorEngine::parse(const std::string& str) {
    if (str.empty()) {
        return Result<Color, SLError>::error(
            {SLErrorCode::InvalidArgument, "Empty color string"});
    }

    // Hex format: #RGB, #RRGGBB, #RRGGBBAA.
    if (str[0] == '#') {
        return Result<Color, SLError>::ok(from_hex(str));
    }

    // Try "rgb(r,g,b)" format.
    if (str.substr(0, 4) == "rgb(") {
        double r, g, b;
        if (std::sscanf(str.c_str(), "rgb(%lf,%lf,%lf)", &r, &g, &b) == 3) {
            if (r > 1.0 || g > 1.0 || b > 1.0) {
                r /= 255.0; g /= 255.0; b /= 255.0;
            }
            return Result<Color, SLError>::ok(
                Color{std::clamp(r, 0.0, 1.0), std::clamp(g, 0.0, 1.0), std::clamp(b, 0.0, 1.0), 1.0});
        }
    }

    // Try "hsl(h,s%,l%)" format.
    if (str.substr(0, 4) == "hsl(") {
        double h, s, l;
        if (std::sscanf(str.c_str(), "hsl(%lf,%lf%%,%lf%%)", &h, &s, &l) == 3) {
            return Result<Color, SLError>::ok(from_hsl({h, s / 100.0, l / 100.0, 1.0}));
        }
    }

    // Try plain hex without #.
    if (str.size() == 6 || str.size() == 3) {
        bool all_hex = true;
        for (char c : str) {
            if (!std::isxdigit(static_cast<unsigned char>(c))) { all_hex = false; break; }
        }
        if (all_hex) {
            return Result<Color, SLError>::ok(from_hex("#" + str));
        }
    }

    return Result<Color, SLError>::error(
        {SLErrorCode::ParseError, "Cannot parse color: " + str});
}

std::string ColorEngine::format(const Color& c, const std::string& space) {
    std::string lower = space;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower == "hex") {
        return to_hex(c);
    } else if (lower == "rgb") {
        std::ostringstream oss;
        oss << "rgb(" << static_cast<int>(std::round(c.r * 255)) << ", "
            << static_cast<int>(std::round(c.g * 255)) << ", "
            << static_cast<int>(std::round(c.b * 255)) << ")";
        return oss.str();
    } else if (lower == "hsl") {
        auto hsl = to_hsl(c);
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1)
            << "hsl(" << hsl.h << ", " << hsl.s * 100.0 << "%, " << hsl.l * 100.0 << "%)";
        return oss.str();
    } else if (lower == "hsv") {
        auto hsv = to_hsv(c);
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1)
            << "hsv(" << hsv.h << ", " << hsv.s * 100.0 << "%, " << hsv.v * 100.0 << "%)";
        return oss.str();
    } else if (lower == "cmyk") {
        auto cmyk = to_cmyk(c);
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1)
            << "cmyk(" << cmyk.c * 100.0 << "%, " << cmyk.m * 100.0 << "%, "
            << cmyk.y * 100.0 << "%, " << cmyk.k * 100.0 << "%)";
        return oss.str();
    } else if (lower == "lab") {
        auto lab = to_lab(c);
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2)
            << "lab(" << lab.L << ", " << lab.a_star << ", " << lab.b_star << ")";
        return oss.str();
    } else if (lower == "oklch") {
        auto oklch = to_oklch(c);
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(4)
            << "oklch(" << oklch.L << ", " << oklch.C << ", " << oklch.h << ")";
        return oss.str();
    }
    return to_hex(c);
}

Result<std::string, SLError> ColorEngine::convert(const std::string& color_str,
                                                   const std::string& /*from_space*/,
                                                   const std::string& to_space) {
    auto parsed = parse(color_str);
    if (!parsed.has_value()) return Result<std::string, SLError>::error(parsed.error());
    return Result<std::string, SLError>::ok(format(parsed.value(), to_space));
}

// ---------------------------------------------------------------------------
// Scheme generation
// ---------------------------------------------------------------------------

std::vector<Color> ColorEngine::generate_scheme(const Color& base, SchemeType type) {
    auto hsl = to_hsl(base);
    std::vector<Color> result;
    result.push_back(base);

    auto rotate = [](double h, double deg) -> double {
        double r = h + deg;
        while (r >= 360.0) r -= 360.0;
        while (r < 0.0) r += 360.0;
        return r;
    };

    switch (type) {
    case SchemeType::Complementary:
        result.push_back(from_hsl({rotate(hsl.h, 180.0), hsl.s, hsl.l, hsl.a}));
        break;

    case SchemeType::Analogous:
        result.push_back(from_hsl({rotate(hsl.h, -30.0), hsl.s, hsl.l, hsl.a}));
        result.push_back(from_hsl({rotate(hsl.h, 30.0), hsl.s, hsl.l, hsl.a}));
        break;

    case SchemeType::Triadic:
        result.push_back(from_hsl({rotate(hsl.h, 120.0), hsl.s, hsl.l, hsl.a}));
        result.push_back(from_hsl({rotate(hsl.h, 240.0), hsl.s, hsl.l, hsl.a}));
        break;

    case SchemeType::SplitComplementary:
        result.push_back(from_hsl({rotate(hsl.h, 150.0), hsl.s, hsl.l, hsl.a}));
        result.push_back(from_hsl({rotate(hsl.h, 210.0), hsl.s, hsl.l, hsl.a}));
        break;

    case SchemeType::Tetradic:
        result.push_back(from_hsl({rotate(hsl.h, 90.0), hsl.s, hsl.l, hsl.a}));
        result.push_back(from_hsl({rotate(hsl.h, 180.0), hsl.s, hsl.l, hsl.a}));
        result.push_back(from_hsl({rotate(hsl.h, 270.0), hsl.s, hsl.l, hsl.a}));
        break;

    case SchemeType::Monochromatic:
        for (int i = 1; i <= 4; ++i) {
            double l = std::clamp(hsl.l + (i - 2) * 0.15, 0.05, 0.95);
            result.push_back(from_hsl({hsl.h, hsl.s, l, hsl.a}));
        }
        break;
    }

    return result;
}

// ---------------------------------------------------------------------------
// Screen color picker
// ---------------------------------------------------------------------------

Result<Color, SLError> ColorEngine::pick_screen_color() const {
    // Take a screenshot via the StrayLight compositor or grim.
    std::string tmp_file = "/tmp/straylight-color-pick.ppm";

    // Try grim (Wayland) first, then scrot (X11), then import (ImageMagick).
    int rc = std::system(("grim -t ppm " + tmp_file + " 2>/dev/null").c_str());
    if (rc != 0) {
        rc = std::system(("scrot -o " + tmp_file + " 2>/dev/null").c_str());
    }
    if (rc != 0) {
        rc = std::system(("import -window root " + tmp_file + " 2>/dev/null").c_str());
    }
    if (rc != 0) {
        return Result<Color, SLError>::error(
            {SLErrorCode::IOError,
             "Cannot capture screen. Install grim (Wayland) or scrot (X11)."});
    }

    // Use slurp or xdotool to get cursor position, then sample the pixel.
    // For now, use slurp to select a point.
    std::string coord_cmd = "slurp -p 2>/dev/null || "
                            "xdotool getmouselocation --shell 2>/dev/null";
    FILE* fp = popen(coord_cmd.c_str(), "r");
    if (!fp) {
        return Result<Color, SLError>::error(
            {SLErrorCode::IOError, "Cannot determine cursor position"});
    }

    char buf[256] = {};
    std::string coord_output;
    while (fgets(buf, sizeof(buf), fp)) {
        coord_output += buf;
    }
    int pclose_rc = pclose(fp);

    // Parse the coordinates — slurp outputs "x,y" on success.
    int px = 0, py = 0;
    if (std::sscanf(coord_output.c_str(), "%d,%d", &px, &py) != 2) {
        // Try xdotool format: X=123\nY=456\n
        std::sscanf(coord_output.c_str(), "X=%d", &px);
        auto ypos = coord_output.find("Y=");
        if (ypos != std::string::npos) {
            std::sscanf(coord_output.c_str() + ypos, "Y=%d", &py);
        }
    }

    // Read the pixel from the PPM file.
    std::ifstream ifs(tmp_file, std::ios::binary);
    if (!ifs) {
        return Result<Color, SLError>::error(
            {SLErrorCode::IOError, "Cannot read screenshot"});
    }

    // Parse PPM header: P6\nwidth height\nmaxval\n
    std::string magic;
    int width = 0, height = 0, maxval = 0;
    ifs >> magic >> width >> height >> maxval;
    ifs.get(); // consume newline

    if (magic != "P6" || width <= 0 || height <= 0 || maxval <= 0) {
        return Result<Color, SLError>::error(
            {SLErrorCode::ParseError, "Invalid PPM screenshot"});
    }

    px = std::clamp(px, 0, width - 1);
    py = std::clamp(py, 0, height - 1);

    // Seek to the pixel.
    size_t bytes_per_pixel = (maxval > 255) ? 6 : 3;
    size_t offset = (static_cast<size_t>(py) * width + px) * bytes_per_pixel;
    ifs.seekg(static_cast<std::streamoff>(offset), std::ios::cur);

    Color c;
    if (maxval > 255) {
        uint8_t pixel[6];
        ifs.read(reinterpret_cast<char*>(pixel), 6);
        c.r = ((pixel[0] << 8) | pixel[1]) / static_cast<double>(maxval);
        c.g = ((pixel[2] << 8) | pixel[3]) / static_cast<double>(maxval);
        c.b = ((pixel[4] << 8) | pixel[5]) / static_cast<double>(maxval);
    } else {
        uint8_t pixel[3];
        ifs.read(reinterpret_cast<char*>(pixel), 3);
        c.r = pixel[0] / 255.0;
        c.g = pixel[1] / 255.0;
        c.b = pixel[2] / 255.0;
    }

    // Clean up.
    std::error_code ec;
    fs::remove(tmp_file, ec);

    return Result<Color, SLError>::ok(c);
}

// ---------------------------------------------------------------------------
// Palette management
// ---------------------------------------------------------------------------

fs::path ColorEngine::palette_dir() {
    const char* home = std::getenv("HOME");
    if (!home) home = "/root";
    return fs::path(home) / ".config" / "straylight" / "palettes";
}

fs::path ColorEngine::palette_path(const std::string& name) {
    return palette_dir() / (name + ".json");
}

Result<void, SLError> ColorEngine::create_palette(const Palette& palette) {
    std::error_code ec;
    fs::create_directories(palette_dir(), ec);
    if (ec) {
        return Result<void, SLError>::error(
            {SLErrorCode::IOError, "Cannot create palette directory: " + ec.message()});
    }

    auto path = palette_path(palette.name);
    std::ofstream ofs(path);
    if (!ofs) {
        return Result<void, SLError>::error(
            {SLErrorCode::IOError, "Cannot write palette file: " + path.string()});
    }

    ofs << "{\n";
    ofs << "  \"name\": \"" << palette.name << "\",\n";
    ofs << "  \"description\": \"" << palette.description << "\",\n";
    ofs << "  \"colors\": [\n";
    for (size_t i = 0; i < palette.colors.size(); ++i) {
        const auto& pc = palette.colors[i];
        ofs << "    {\"name\": \"" << pc.name << "\", \"hex\": \""
            << to_hex(pc.color) << "\"}";
        if (i + 1 < palette.colors.size()) ofs << ",";
        ofs << "\n";
    }
    ofs << "  ]\n}\n";

    SL_INFO("color: created palette '{}'", palette.name);
    return Result<void, SLError>::ok();
}

Result<std::vector<std::string>, SLError> ColorEngine::list_palettes() const {
    std::vector<std::string> names;
    auto dir = palette_dir();
    if (!fs::exists(dir)) return Result<std::vector<std::string>, SLError>::ok(std::move(names));

    std::error_code ec;
    for (auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.path().extension() == ".json") {
            names.push_back(entry.path().stem().string());
        }
    }
    std::sort(names.begin(), names.end());
    return Result<std::vector<std::string>, SLError>::ok(std::move(names));
}

Result<Palette, SLError> ColorEngine::load_palette(const std::string& name) const {
    auto path = palette_path(name);
    std::ifstream ifs(path);
    if (!ifs) {
        return Result<Palette, SLError>::error(
            {SLErrorCode::NotFound, "Palette not found: " + name});
    }

    // Simple JSON parser for our known format.
    std::string content((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());

    Palette palette;
    palette.name = name;

    // Extract description.
    auto desc_pos = content.find("\"description\"");
    if (desc_pos != std::string::npos) {
        auto colon = content.find(':', desc_pos);
        auto q1 = content.find('"', colon + 1);
        auto q2 = content.find('"', q1 + 1);
        if (q1 != std::string::npos && q2 != std::string::npos) {
            palette.description = content.substr(q1 + 1, q2 - q1 - 1);
        }
    }

    // Extract colors.
    auto colors_pos = content.find("\"colors\"");
    if (colors_pos != std::string::npos) {
        size_t search_from = colors_pos;
        while (true) {
            auto name_pos = content.find("\"name\"", search_from);
            if (name_pos == std::string::npos) break;

            auto colon1 = content.find(':', name_pos);
            auto q1 = content.find('"', colon1 + 1);
            auto q2 = content.find('"', q1 + 1);
            if (q1 == std::string::npos || q2 == std::string::npos) break;
            std::string color_name = content.substr(q1 + 1, q2 - q1 - 1);

            auto hex_pos = content.find("\"hex\"", q2);
            if (hex_pos == std::string::npos) break;
            auto colon2 = content.find(':', hex_pos);
            auto h1 = content.find('"', colon2 + 1);
            auto h2 = content.find('"', h1 + 1);
            if (h1 == std::string::npos || h2 == std::string::npos) break;
            std::string hex_val = content.substr(h1 + 1, h2 - h1 - 1);

            PaletteColor pc;
            pc.name = color_name;
            pc.color = from_hex(hex_val);
            palette.colors.push_back(pc);

            search_from = h2 + 1;
        }
    }

    return Result<Palette, SLError>::ok(std::move(palette));
}

Result<void, SLError> ColorEngine::delete_palette(const std::string& name) {
    auto path = palette_path(name);
    std::error_code ec;
    if (!fs::exists(path)) {
        return Result<void, SLError>::error(
            {SLErrorCode::NotFound, "Palette not found: " + name});
    }
    fs::remove(path, ec);
    if (ec) {
        return Result<void, SLError>::error(
            {SLErrorCode::IOError, "Cannot delete palette: " + ec.message()});
    }
    return Result<void, SLError>::ok();
}

// ---------------------------------------------------------------------------
// Export
// ---------------------------------------------------------------------------

std::string ColorEngine::export_css(const Palette& palette) {
    std::ostringstream oss;
    oss << ":root {\n";
    for (const auto& pc : palette.colors) {
        std::string var_name = pc.name;
        // Sanitize: lowercase, replace spaces with dashes.
        std::transform(var_name.begin(), var_name.end(), var_name.begin(), ::tolower);
        for (auto& c : var_name) {
            if (c == ' ') c = '-';
        }
        oss << "  --" << var_name << ": " << to_hex(pc.color) << ";\n";
    }
    oss << "}\n";
    return oss.str();
}

std::string ColorEngine::export_scss(const Palette& palette) {
    std::ostringstream oss;
    for (const auto& pc : palette.colors) {
        std::string var_name = pc.name;
        std::transform(var_name.begin(), var_name.end(), var_name.begin(), ::tolower);
        for (auto& c : var_name) {
            if (c == ' ') c = '-';
        }
        oss << "$" << var_name << ": " << to_hex(pc.color) << ";\n";
    }
    return oss.str();
}

std::string ColorEngine::export_json(const Palette& palette) {
    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"name\": \"" << palette.name << "\",\n";
    oss << "  \"colors\": {\n";
    for (size_t i = 0; i < palette.colors.size(); ++i) {
        const auto& pc = palette.colors[i];
        auto hsl = to_hsl(pc.color);
        oss << "    \"" << pc.name << "\": {\n"
            << "      \"hex\": \"" << to_hex(pc.color) << "\",\n"
            << "      \"rgb\": [" << static_cast<int>(std::round(pc.color.r * 255)) << ", "
            << static_cast<int>(std::round(pc.color.g * 255)) << ", "
            << static_cast<int>(std::round(pc.color.b * 255)) << "],\n"
            << "      \"hsl\": [" << std::fixed << std::setprecision(1)
            << hsl.h << ", " << hsl.s * 100.0 << ", " << hsl.l * 100.0 << "]\n"
            << "    }";
        if (i + 1 < palette.colors.size()) oss << ",";
        oss << "\n";
    }
    oss << "  }\n}\n";
    return oss.str();
}

std::string ColorEngine::export_svg(const Palette& palette) {
    int swatch_size = 60;
    int cols = static_cast<int>(palette.colors.size());
    int width = cols * swatch_size;
    int height = swatch_size + 30;

    std::ostringstream oss;
    oss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << width
        << "\" height=\"" << height << "\">\n";

    for (int i = 0; i < cols; ++i) {
        const auto& pc = palette.colors[i];
        int x = i * swatch_size;
        oss << "  <rect x=\"" << x << "\" y=\"0\" width=\"" << swatch_size
            << "\" height=\"" << swatch_size << "\" fill=\"" << to_hex(pc.color) << "\"/>\n";
        oss << "  <text x=\"" << (x + swatch_size / 2) << "\" y=\"" << (swatch_size + 16)
            << "\" font-size=\"9\" text-anchor=\"middle\" fill=\"#333\">"
            << pc.name << "</text>\n";
    }

    oss << "</svg>\n";
    return oss.str();
}

Result<std::string, SLError> ColorEngine::export_palette(const Palette& palette,
                                                          const std::string& fmt) {
    std::string lower = fmt;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower == "css")  return Result<std::string, SLError>::ok(export_css(palette));
    if (lower == "scss") return Result<std::string, SLError>::ok(export_scss(palette));
    if (lower == "json") return Result<std::string, SLError>::ok(export_json(palette));
    if (lower == "svg")  return Result<std::string, SLError>::ok(export_svg(palette));

    return Result<std::string, SLError>::error(
        {SLErrorCode::InvalidArgument,
         "Unknown export format: " + fmt + " (valid: css, scss, json, svg)"});
}

// ---------------------------------------------------------------------------
// Theme integration
// ---------------------------------------------------------------------------

Result<void, SLError> ColorEngine::apply_theme(const Palette& palette) const {
    const char* home = std::getenv("HOME");
    if (!home) home = "/root";

    fs::path theme_dir = fs::path(home) / ".config" / "straylight" / "theme";
    std::error_code ec;
    fs::create_directories(theme_dir, ec);

    fs::path theme_file = theme_dir / "colors.json";

    // Build a theme JSON from the palette.
    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"palette\": \"" << palette.name << "\",\n";
    oss << "  \"colors\": {\n";
    for (size_t i = 0; i < palette.colors.size(); ++i) {
        const auto& pc = palette.colors[i];
        oss << "    \"" << pc.name << "\": \"" << to_hex(pc.color) << "\"";
        if (i + 1 < palette.colors.size()) oss << ",";
        oss << "\n";
    }
    oss << "  }\n}\n";

    std::ofstream ofs(theme_file, std::ios::trunc);
    if (!ofs) {
        return Result<void, SLError>::error(
            {SLErrorCode::IOError, "Cannot write theme file: " + theme_file.string()});
    }
    ofs << oss.str();

    SL_INFO("color: applied palette '{}' as theme", palette.name);
    return Result<void, SLError>::ok();
}

} // namespace straylight
