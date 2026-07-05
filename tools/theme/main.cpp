// tools/theme/main.cpp
// straylight-theme — theme engine CLI.
#include "theme_engine.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

static void print_usage() {
    std::cerr
        << "straylight-theme — visual theme manager\n"
        << "\n"
        << "Usage:\n"
        << "  straylight-theme list                      List installed themes\n"
        << "  straylight-theme apply <name>              Apply a theme\n"
        << "  straylight-theme create <name> [--base=X]  Create a new theme\n"
        << "  straylight-theme preview <name>            Preview colors in terminal\n"
        << "  straylight-theme edit <name>               Show theme file path for editing\n"
        << "  straylight-theme export <name> [--output <file>]\n"
        << "  straylight-theme import <file>\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string command = argv[1];
    if (command == "--help" || command == "-h") {
        print_usage();
        return 0;
    }

    straylight::ThemeEngine engine;

    // -----------------------------------------------------------------------
    // list
    // -----------------------------------------------------------------------
    if (command == "list") {
        auto themes = engine.list();
        if (themes.empty()) {
            std::cout << "No themes found.\n";
            return 0;
        }
        std::string active = engine.active_theme();
        std::cout << "Installed themes:\n";
        for (const auto& name : themes) {
            auto theme = engine.load(name);
            if (name == active) {
                std::cout << "  \033[32m* " << name << "\033[0m";
            } else {
                std::cout << "    " << name;
            }
            if (theme.has_value() && !theme.value().description.empty()) {
                std::cout << " — " << theme.value().description;
            }
            std::cout << "\n";
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // apply <name>
    // -----------------------------------------------------------------------
    if (command == "apply") {
        if (argc < 3) {
            std::cerr << "Error: 'apply' requires a theme name\n";
            return 1;
        }
        std::cout << "Applying theme '" << argv[2] << "'...\n";
        auto res = engine.apply(argv[2]);
        if (res.has_value()) {
            std::cout << "\033[32mTheme applied.\033[0m\n";
            std::cout << "Restart the compositor or logout to see full effect.\n";
        } else {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // create <name>
    // -----------------------------------------------------------------------
    if (command == "create") {
        if (argc < 3) {
            std::cerr << "Error: 'create' requires a theme name\n";
            return 1;
        }
        std::string base = "default";
        for (int i = 3; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg.rfind("--base=", 0) == 0) {
                base = arg.substr(7);
            }
        }
        auto res = engine.create(argv[2], base);
        if (res.has_value()) {
            std::cout << "\033[32mTheme '" << argv[2] << "' created.\033[0m\n";
            std::cout << "Edit it with: straylight-theme edit " << argv[2] << "\n";
        } else {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // preview <name>
    // -----------------------------------------------------------------------
    if (command == "preview") {
        if (argc < 3) {
            std::cerr << "Error: 'preview' requires a theme name\n";
            return 1;
        }
        std::cout << engine.preview(argv[2]);
        return 0;
    }

    // -----------------------------------------------------------------------
    // edit <name>
    // -----------------------------------------------------------------------
    if (command == "edit") {
        if (argc < 3) {
            std::cerr << "Error: 'edit' requires a theme name\n";
            return 1;
        }
        const char* home = std::getenv("HOME");
        std::string user_dir;
        if (home) {
            user_dir = std::string(home) + "/.local/share/straylight/themes";
        }
        std::string path = user_dir + "/" + argv[2] + "/theme.json";
        if (!std::filesystem::exists(path)) {
            path = "/usr/share/straylight/themes/" + std::string(argv[2]) + "/theme.json";
        }
        if (std::filesystem::exists(path)) {
            std::cout << "Theme file: " << path << "\n";
            const char* editor = std::getenv("EDITOR");
            if (!editor) editor = "nano";
            std::cout << "Open with: " << editor << " " << path << "\n";
        } else {
            std::cerr << "Theme not found. Create it first with: straylight-theme create "
                      << argv[2] << "\n";
            return 1;
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // export <name>
    // -----------------------------------------------------------------------
    if (command == "export") {
        if (argc < 3) {
            std::cerr << "Error: 'export' requires a theme name\n";
            return 1;
        }
        std::string output;
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
                output = argv[++i];
            }
        }
        auto res = engine.export_json(argv[2]);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        if (output.empty()) {
            std::cout << res.value() << "\n";
        } else {
            std::ofstream ofs(output);
            if (!ofs) {
                std::cerr << "Error: cannot write to " << output << "\n";
                return 1;
            }
            ofs << res.value() << "\n";
            std::cout << "Exported to " << output << "\n";
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // import <file>
    // -----------------------------------------------------------------------
    if (command == "import") {
        if (argc < 3) {
            std::cerr << "Error: 'import' requires a file path\n";
            return 1;
        }
        std::ifstream ifs(argv[2]);
        if (!ifs) {
            std::cerr << "Error: cannot read " << argv[2] << "\n";
            return 1;
        }
        std::string content((std::istreambuf_iterator<char>(ifs)),
                             std::istreambuf_iterator<char>());
        auto res = engine.import_json(content);
        if (res.has_value()) {
            std::cout << "Theme imported.\n";
        } else {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        return 0;
    }

    std::cerr << "Unknown command: " << command << "\n";
    print_usage();
    return 1;
}
