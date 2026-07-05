// tools/locale/main.cpp
// straylight-locale — locale and language manager CLI.
#include "locale_manager.h"

#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>

static void print_usage() {
    std::cerr
        << "straylight-locale — locale and language manager\n"
        << "\n"
        << "Usage:\n"
        << "  straylight-locale status                  Show current locale settings\n"
        << "  straylight-locale set-language <lang>      Set system language\n"
        << "  straylight-locale set-timezone <tz>        Set timezone\n"
        << "  straylight-locale set-formats <locale>     Set date/number/currency format\n"
        << "  straylight-locale set-keyboard <layout> [variant]\n"
        << "  straylight-locale list-languages           List available languages\n"
        << "  straylight-locale list-timezones [region]  List available timezones\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        // Default: show status
        straylight::LocaleManager mgr;
        auto s = mgr.status();
        std::cout << "\033[1;36mLocale Settings\033[0m\n"
                  << "  Language:        " << s.language << "\n"
                  << "  Timezone:        " << s.timezone << "\n"
                  << "  Date format:     " << s.date_format << "\n"
                  << "  Number format:   " << s.number_format << "\n"
                  << "  Currency format: " << s.currency_format << "\n"
                  << "  Keyboard layout: " << s.keyboard_layout;
        if (!s.keyboard_variant.empty()) {
            std::cout << " (" << s.keyboard_variant << ")";
        }
        std::cout << "\n";
        return 0;
    }

    std::string command = argv[1];
    if (command == "--help" || command == "-h") {
        print_usage();
        return 0;
    }

    straylight::LocaleManager mgr;

    // -----------------------------------------------------------------------
    // status
    // -----------------------------------------------------------------------
    if (command == "status") {
        auto s = mgr.status();
        std::cout << "\033[1;36mLocale Settings\033[0m\n"
                  << "  Language:        " << s.language << "\n"
                  << "  Timezone:        " << s.timezone << "\n"
                  << "  Date format:     " << s.date_format << "\n"
                  << "  Number format:   " << s.number_format << "\n"
                  << "  Currency format: " << s.currency_format << "\n"
                  << "  Keyboard layout: " << s.keyboard_layout;
        if (!s.keyboard_variant.empty()) {
            std::cout << " (" << s.keyboard_variant << ")";
        }
        std::cout << "\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // set-language <lang>
    // -----------------------------------------------------------------------
    if (command == "set-language") {
        if (argc < 3) {
            std::cerr << "Error: 'set-language' requires a language code\n";
            return 1;
        }
        auto res = mgr.set_language(argv[2]);
        if (res.has_value()) {
            std::cout << "\033[32mLanguage set to " << argv[2] << "\033[0m\n";
            std::cout << "Log out and back in for full effect.\n";
        } else {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // set-timezone <tz>
    // -----------------------------------------------------------------------
    if (command == "set-timezone") {
        if (argc < 3) {
            std::cerr << "Error: 'set-timezone' requires a timezone\n";
            return 1;
        }
        auto res = mgr.set_timezone(argv[2]);
        if (res.has_value()) {
            std::cout << "\033[32mTimezone set to " << argv[2] << "\033[0m\n";
        } else {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // set-formats <locale>
    // -----------------------------------------------------------------------
    if (command == "set-formats") {
        if (argc < 3) {
            std::cerr << "Error: 'set-formats' requires a locale code\n";
            return 1;
        }
        auto res = mgr.set_formats(argv[2]);
        if (res.has_value()) {
            std::cout << "\033[32mFormat preferences set to " << argv[2] << "\033[0m\n";
        } else {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // set-keyboard <layout> [variant]
    // -----------------------------------------------------------------------
    if (command == "set-keyboard") {
        if (argc < 3) {
            std::cerr << "Error: 'set-keyboard' requires a layout\n";
            return 1;
        }
        std::string variant;
        if (argc >= 4) variant = argv[3];
        auto res = mgr.set_keyboard(argv[2], variant);
        if (res.has_value()) {
            std::cout << "\033[32mKeyboard layout set to " << argv[2];
            if (!variant.empty()) std::cout << " (" << variant << ")";
            std::cout << "\033[0m\n";
        } else {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // list-languages
    // -----------------------------------------------------------------------
    if (command == "list-languages") {
        auto languages = mgr.list_languages();
        std::cout << "Available languages:\n";
        for (const auto& l : languages) {
            printf("  \033[36m%-10s\033[0m  %s\n", l.code.c_str(), l.name.c_str());
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // list-timezones [region]
    // -----------------------------------------------------------------------
    if (command == "list-timezones") {
        std::string region;
        if (argc >= 3) region = argv[2];
        auto timezones = mgr.list_timezones(region);
        if (timezones.empty()) {
            std::cout << "No timezones found";
            if (!region.empty()) std::cout << " for region '" << region << "'";
            std::cout << ".\n";
            return 0;
        }
        for (const auto& tz : timezones) {
            std::cout << "  " << tz << "\n";
        }
        std::cout << "\nTotal: " << timezones.size() << " timezones\n";
        return 0;
    }

    std::cerr << "Unknown command: " << command << "\n";
    print_usage();
    return 1;
}
