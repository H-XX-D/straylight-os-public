// tools/color/main.cpp
// straylight-color — Color picker and palette manager for StrayLight OS.
#include "color_engine.h"

#include <straylight/log.h>

#include <iostream>
#include <sstream>
#include <string>

using straylight::ColorEngine;
using straylight::Palette;
using straylight::PaletteColor;

static void print_usage() {
    std::cerr
        << "straylight-color — Color picker & palette manager\n\n"
        << "Usage:\n"
        << "  straylight-color pick                                Screen color picker\n"
        << "  straylight-color palette create <name> <hex>...      Create a palette\n"
        << "  straylight-color palette list                        List palettes\n"
        << "  straylight-color palette show <name>                 Show palette\n"
        << "  straylight-color palette delete <name>               Delete palette\n"
        << "  straylight-color convert <color> <from> <to>         Convert between spaces\n"
        << "  straylight-color generate <type> <base-color>        Generate color scheme\n"
        << "  straylight-color export <palette> <format>           Export palette\n"
        << "  straylight-color theme <palette>                     Apply to theme system\n"
        << "\nColor spaces: hex, rgb, hsl, hsv, cmyk, lab, oklch\n"
        << "Scheme types: complementary, analogous, triadic, split-complementary,\n"
        << "              tetradic, monochromatic\n"
        << "Export formats: css, scss, json, svg\n"
        << "\nExamples:\n"
        << "  straylight-color convert '#ff6600' hex hsl\n"
        << "  straylight-color generate triadic '#3498db'\n"
        << "  straylight-color palette create ocean '#1a535c' '#4ecdc4' '#f7fff7' '#ff6b6b'\n"
        << "  straylight-color export ocean css\n";
}

/// Print a color swatch in the terminal.
static void print_swatch(const straylight::Color& c, const std::string& label = "") {
    int r = static_cast<int>(c.r * 255);
    int g = static_cast<int>(c.g * 255);
    int b = static_cast<int>(c.b * 255);
    // 24-bit color escape: background.
    std::cout << "\033[48;2;" << r << ";" << g << ";" << b << "m"
              << "      "
              << "\033[0m "
              << ColorEngine::to_hex(c);
    if (!label.empty()) std::cout << "  " << label;
    std::cout << "\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string cmd = argv[1];

    if (cmd == "--help" || cmd == "-h") {
        print_usage();
        return 0;
    }

    ColorEngine engine;

    if (cmd == "pick") {
        auto r = engine.pick_screen_color();
        if (!r.has_value()) {
            std::cerr << "Error: " << r.error().message() << "\n";
            return 1;
        }
        const auto& c = r.value();
        std::cout << "Picked color:\n";
        print_swatch(c);
        std::cout << "\n";
        std::cout << "  hex:   " << ColorEngine::to_hex(c) << "\n";
        std::cout << "  rgb:   " << ColorEngine::format(c, "rgb") << "\n";
        std::cout << "  hsl:   " << ColorEngine::format(c, "hsl") << "\n";
        std::cout << "  hsv:   " << ColorEngine::format(c, "hsv") << "\n";
        std::cout << "  cmyk:  " << ColorEngine::format(c, "cmyk") << "\n";
        std::cout << "  lab:   " << ColorEngine::format(c, "lab") << "\n";
        std::cout << "  oklch: " << ColorEngine::format(c, "oklch") << "\n";

    } else if (cmd == "palette") {
        if (argc < 3) {
            std::cerr << "Usage: straylight-color palette <create|list|show|delete> ...\n";
            return 1;
        }
        std::string subcmd = argv[2];

        if (subcmd == "create") {
            if (argc < 5) {
                std::cerr << "Usage: straylight-color palette create <name> <hex>...\n";
                return 1;
            }
            Palette pal;
            pal.name = argv[3];
            for (int i = 4; i < argc; ++i) {
                PaletteColor pc;
                pc.name = "color-" + std::to_string(i - 3);
                auto parsed = ColorEngine::parse(argv[i]);
                if (!parsed.has_value()) {
                    std::cerr << "Error: cannot parse color '" << argv[i]
                              << "': " << parsed.error().message() << "\n";
                    return 1;
                }
                pc.color = parsed.value();
                pal.colors.push_back(pc);
            }
            auto r = engine.create_palette(pal);
            if (!r.has_value()) {
                std::cerr << "Error: " << r.error().message() << "\n";
                return 1;
            }
            std::cout << "Palette '" << pal.name << "' created with "
                      << pal.colors.size() << " colors.\n";

        } else if (subcmd == "list") {
            auto r = engine.list_palettes();
            if (!r.has_value()) {
                std::cerr << "Error: " << r.error().message() << "\n";
                return 1;
            }
            const auto& names = r.value();
            if (names.empty()) {
                std::cout << "No palettes found.\n";
                return 0;
            }
            std::cout << "Palettes:\n";
            for (const auto& name : names) {
                std::cout << "  " << name << "\n";
            }

        } else if (subcmd == "show") {
            if (argc < 4) {
                std::cerr << "Usage: straylight-color palette show <name>\n";
                return 1;
            }
            auto r = engine.load_palette(argv[3]);
            if (!r.has_value()) {
                std::cerr << "Error: " << r.error().message() << "\n";
                return 1;
            }
            const auto& pal = r.value();
            std::cout << "Palette: " << pal.name << "\n";
            if (!pal.description.empty())
                std::cout << "Description: " << pal.description << "\n";
            std::cout << "\n";
            for (const auto& pc : pal.colors) {
                print_swatch(pc.color, pc.name);
            }

        } else if (subcmd == "delete") {
            if (argc < 4) {
                std::cerr << "Usage: straylight-color palette delete <name>\n";
                return 1;
            }
            auto r = engine.delete_palette(argv[3]);
            if (!r.has_value()) {
                std::cerr << "Error: " << r.error().message() << "\n";
                return 1;
            }
            std::cout << "Palette '" << argv[3] << "' deleted.\n";

        } else {
            std::cerr << "Unknown palette command: " << subcmd << "\n";
            return 1;
        }

    } else if (cmd == "convert") {
        if (argc < 5) {
            std::cerr << "Usage: straylight-color convert <color> <from-space> <to-space>\n";
            return 1;
        }
        auto r = ColorEngine::convert(argv[2], argv[3], argv[4]);
        if (!r.has_value()) {
            std::cerr << "Error: " << r.error().message() << "\n";
            return 1;
        }
        // Also show a swatch.
        auto parsed = ColorEngine::parse(argv[2]);
        if (parsed.has_value()) {
            print_swatch(parsed.value());
            std::cout << "\n";
        }
        std::cout << r.value() << "\n";

    } else if (cmd == "generate") {
        if (argc < 4) {
            std::cerr << "Usage: straylight-color generate <scheme-type> <base-color>\n";
            return 1;
        }
        auto scheme_result = straylight::parse_scheme_type(argv[2]);
        if (!scheme_result.has_value()) {
            std::cerr << "Error: " << scheme_result.error().message() << "\n";
            return 1;
        }
        auto base_result = ColorEngine::parse(argv[3]);
        if (!base_result.has_value()) {
            std::cerr << "Error: " << base_result.error().message() << "\n";
            return 1;
        }

        auto colors = ColorEngine::generate_scheme(base_result.value(), scheme_result.value());
        std::cout << "Generated " << argv[2] << " scheme from " << argv[3] << ":\n\n";
        for (size_t i = 0; i < colors.size(); ++i) {
            std::string label = (i == 0) ? "(base)" : "";
            print_swatch(colors[i], label);
        }

    } else if (cmd == "export") {
        if (argc < 4) {
            std::cerr << "Usage: straylight-color export <palette> <format>\n";
            return 1;
        }
        auto pal_result = engine.load_palette(argv[2]);
        if (!pal_result.has_value()) {
            std::cerr << "Error: " << pal_result.error().message() << "\n";
            return 1;
        }
        auto r = ColorEngine::export_palette(pal_result.value(), argv[3]);
        if (!r.has_value()) {
            std::cerr << "Error: " << r.error().message() << "\n";
            return 1;
        }
        std::cout << r.value();

    } else if (cmd == "theme") {
        if (argc < 3) {
            std::cerr << "Usage: straylight-color theme <palette>\n";
            return 1;
        }
        auto pal_result = engine.load_palette(argv[2]);
        if (!pal_result.has_value()) {
            std::cerr << "Error: " << pal_result.error().message() << "\n";
            return 1;
        }
        auto r = engine.apply_theme(pal_result.value());
        if (!r.has_value()) {
            std::cerr << "Error: " << r.error().message() << "\n";
            return 1;
        }
        std::cout << "Theme updated with palette '" << argv[2] << "'.\n";

    } else {
        std::cerr << "Error: unknown command '" << cmd << "'\n";
        print_usage();
        return 1;
    }

    return 0;
}
