// tools/fonts/main.cpp
// straylight-fonts — Font manager for StrayLight OS.
#include "font_manager.h"

#include <straylight/log.h>

#include <cstdlib>
#include <iostream>
#include <string>

using straylight::FontManager;

static void print_usage() {
    std::cerr
        << "straylight-fonts — Font manager\n\n"
        << "Usage:\n"
        << "  straylight-fonts list [--family=X]              List installed fonts\n"
        << "  straylight-fonts install <file|google:name>     Install a font\n"
        << "  straylight-fonts remove <family>                Remove a font family\n"
        << "  straylight-fonts preview <family> [--text=X]    Preview a font\n"
        << "  straylight-fonts search <query>                 Search Google Fonts\n"
        << "  straylight-fonts info <family>                  Show font details\n"
        << "  straylight-fonts set-default <family> <cat>     Set default for category\n"
        << "  straylight-fonts export                         Export font list as JSON\n"
        << "\nCategories for set-default: sans-serif, serif, monospace\n"
        << "\nExamples:\n"
        << "  straylight-fonts list --family=Noto\n"
        << "  straylight-fonts install google:Inter\n"
        << "  straylight-fonts install /tmp/MyFont.ttf\n"
        << "  straylight-fonts preview \"Fira Code\" --text=\"Hello World\"\n"
        << "  straylight-fonts search monospace\n"
        << "  straylight-fonts set-default Inter sans-serif\n";
}

/// Extract value from --key=value arg.
static std::string extract_flag(int argc, char* argv[], const std::string& prefix) {
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.substr(0, prefix.size()) == prefix) {
            return arg.substr(prefix.size());
        }
    }
    return "";
}

/// Collect all positional args (those not starting with --) after index `start`.
static std::string positional_arg(int argc, char* argv[], int index) {
    if (index < argc) return argv[index];
    return "";
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

    FontManager mgr;
    auto scan_result = mgr.scan();
    if (!scan_result.has_value()) {
        std::cerr << "Warning: font scan failed: " << scan_result.error().message() << "\n";
    }

    if (cmd == "list") {
        std::string family_filter = extract_flag(argc, argv, "--family=");
        auto fonts = mgr.list(family_filter);

        if (fonts.empty()) {
            std::cout << "No fonts found";
            if (!family_filter.empty()) std::cout << " matching '" << family_filter << "'";
            std::cout << ".\n";
            return 0;
        }

        // Group by family for display.
        std::string last_family;
        for (const auto& f : fonts) {
            if (f.family != last_family) {
                if (!last_family.empty()) std::cout << "\n";
                std::cout << "\033[1m" << f.family << "\033[0m\n";
                last_family = f.family;
            }
            std::cout << "  " << f.style << " (w" << f.weight << ")";
            if (f.is_variable) std::cout << " [variable]";
            std::cout << "  " << f.path << "\n";
        }
        std::cout << "\nTotal: " << fonts.size() << " font files\n";

    } else if (cmd == "install") {
        if (argc < 3) {
            std::cerr << "Usage: straylight-fonts install <file|google:family>\n";
            return 1;
        }
        std::string source = argv[2];
        auto r = mgr.install(source);
        if (!r.has_value()) {
            std::cerr << "Error: " << r.error().message() << "\n";
            return 1;
        }
        std::cout << "Font installed successfully.\n";

    } else if (cmd == "remove") {
        if (argc < 3) {
            std::cerr << "Usage: straylight-fonts remove <family>\n";
            return 1;
        }
        std::string family = argv[2];
        auto r = mgr.remove(family);
        if (!r.has_value()) {
            std::cerr << "Error: " << r.error().message() << "\n";
            return 1;
        }
        std::cout << "Font family '" << family << "' removed.\n";

    } else if (cmd == "preview") {
        if (argc < 3) {
            std::cerr << "Usage: straylight-fonts preview <family> [--text=X]\n";
            return 1;
        }
        std::string family = argv[2];
        std::string text = extract_flag(argc, argv, "--text=");
        if (text.empty()) text = "The quick brown fox jumps over the lazy dog";

        auto r = mgr.preview(family, text);
        if (!r.has_value()) {
            std::cerr << "Error: " << r.error().message() << "\n";
            return 1;
        }
        std::cout << r.value();

    } else if (cmd == "search") {
        if (argc < 3) {
            std::cerr << "Usage: straylight-fonts search <query>\n";
            return 1;
        }
        std::string query = argv[2];
        auto r = mgr.search(query);
        if (!r.has_value()) {
            std::cerr << "Error: " << r.error().message() << "\n";
            return 1;
        }

        const auto& results = r.value();
        if (results.empty()) {
            std::cout << "No fonts found matching '" << query << "'.\n";
            return 0;
        }

        std::cout << "Google Fonts matching '" << query << "':\n\n";
        for (const auto& entry : results) {
            std::cout << "  \033[1m" << entry.family << "\033[0m"
                      << "  [" << entry.category << "]"
                      << "  variants: " << entry.variants.size() << "\n";
        }
        std::cout << "\nInstall with: straylight-fonts install google:<family>\n";

    } else if (cmd == "info") {
        if (argc < 3) {
            std::cerr << "Usage: straylight-fonts info <family>\n";
            return 1;
        }
        std::string family = argv[2];
        auto r = mgr.info(family);
        if (!r.has_value()) {
            std::cerr << "Error: " << r.error().message() << "\n";
            return 1;
        }

        const auto& fonts = r.value();
        std::cout << "Font Family: " << family << "\n";
        std::cout << "Variants:    " << fonts.size() << "\n\n";

        for (const auto& f : fonts) {
            std::cout << "  Style:    " << f.style << "\n";
            std::cout << "  Weight:   " << f.weight << "\n";
            std::cout << "  Format:   " << f.format << "\n";
            if (!f.version.empty())
                std::cout << "  Version:  " << f.version << "\n";
            if (f.is_variable)
                std::cout << "  Type:     Variable font\n";
            std::cout << "  Path:     " << f.path << "\n";
            if (!f.license.empty())
                std::cout << "  License:  " << f.license.substr(0, 80) << "\n";
            std::cout << "\n";
        }

    } else if (cmd == "set-default") {
        if (argc < 4) {
            std::cerr << "Usage: straylight-fonts set-default <family> <category>\n";
            std::cerr << "Categories: sans-serif, serif, monospace\n";
            return 1;
        }
        std::string family = argv[2];
        std::string category = argv[3];

        auto r = mgr.set_default(family, category);
        if (!r.has_value()) {
            std::cerr << "Error: " << r.error().message() << "\n";
            return 1;
        }
        std::cout << "Default " << category << " font set to '" << family << "'.\n";

    } else if (cmd == "export") {
        std::cout << mgr.export_json();

    } else {
        std::cerr << "Error: unknown command '" << cmd << "'\n";
        print_usage();
        return 1;
    }

    return 0;
}
