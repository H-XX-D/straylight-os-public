// tools/display/main.cpp
// CLI front-end for straylight-display — monitor configuration and layout.

#include "display_manager.h"

#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

static void print_usage() {
    std::cerr
        << "straylight-display — monitor configuration & layout\n"
        << "\n"
        << "Usage:\n"
        << "  straylight-display list                                    List connected outputs\n"
        << "  straylight-display set <output> --res=WxH [--rate=N] [--pos=X,Y]  Configure output\n"
        << "  straylight-display profile save <name>                     Save current layout\n"
        << "  straylight-display profile load <name>                     Apply saved layout\n"
        << "  straylight-display profile list                            List saved profiles\n"
        << "  straylight-display profile delete <name>                   Delete a profile\n"
        << "  straylight-display mirror <source> <dest>                  Mirror outputs\n"
        << "  straylight-display rotate <output> <degrees>               Rotate output (0/90/180/270)\n"
        << "  straylight-display night [--enable|--disable] [--temp=K] [--schedule=S]\n"
        << "                                                             Night mode / color temp\n"
        << "  straylight-display icc <output> <profile.icc>             Load ICC color profile\n";
}

/// Parse --key=value style arguments.
static std::string get_arg(int argc, char* argv[], const std::string& prefix, int start = 2) {
    for (int i = start; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind(prefix, 0) == 0) {
            return arg.substr(prefix.size());
        }
    }
    return "";
}

static bool has_flag(int argc, char* argv[], const std::string& flag, int start = 2) {
    for (int i = start; i < argc; ++i) {
        if (std::string(argv[i]) == flag) return true;
    }
    return false;
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

    straylight::DisplayManager mgr;

    // -----------------------------------------------------------------------
    // list
    // -----------------------------------------------------------------------
    if (command == "list") {
        auto res = mgr.list_outputs();
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }

        const auto& outputs = res.value();
        if (outputs.empty()) {
            std::cout << "No outputs detected.\n";
            return 0;
        }

        for (const auto& out : outputs) {
            std::cout << out.name;
            if (!out.connected) {
                std::cout << " (disconnected)\n";
                continue;
            }
            std::cout << " — ";
            if (!out.make.empty()) std::cout << out.make << " ";
            if (!out.model.empty()) std::cout << out.model;
            if (!out.serial.empty()) std::cout << " [" << out.serial << "]";
            std::cout << "\n";

            if (out.enabled) {
                std::cout << "  Active:   " << out.active_mode.width << "x"
                          << out.active_mode.height << " @ "
                          << std::fixed << std::setprecision(1)
                          << out.active_mode.refresh_hz << " Hz\n";
                std::cout << "  Position: " << out.pos_x << "," << out.pos_y << "\n";
                if (out.rotation_deg != 0) {
                    std::cout << "  Rotation: " << out.rotation_deg << " deg\n";
                }
            } else {
                std::cout << "  (disabled)\n";
            }

            if (!out.modes.empty()) {
                std::cout << "  Modes:\n";
                for (const auto& m : out.modes) {
                    std::cout << "    " << m.width << "x" << m.height
                              << " @ " << std::fixed << std::setprecision(2)
                              << m.refresh_hz << " Hz";
                    if (m.current) std::cout << " *";
                    if (m.preferred) std::cout << " (preferred)";
                    std::cout << "\n";
                }
            }
            std::cout << "\n";
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // set <output> --res=WxH [--rate=N] [--pos=X,Y]
    // -----------------------------------------------------------------------
    if (command == "set") {
        if (argc < 3) {
            std::cerr << "Error: 'set' requires an output name\n";
            return 1;
        }
        std::string output = argv[2];

        std::string res_str = get_arg(argc, argv, "--res=", 3);
        std::string rate_str = get_arg(argc, argv, "--rate=", 3);
        std::string pos_str = get_arg(argc, argv, "--pos=", 3);

        if (!res_str.empty()) {
            int w = 0, h = 0;
            if (sscanf(res_str.c_str(), "%dx%d", &w, &h) != 2) {
                std::cerr << "Error: invalid resolution format, expected WxH\n";
                return 1;
            }
            double rate = rate_str.empty() ? 0.0 : std::stod(rate_str);
            auto result = mgr.set_mode(output, w, h, rate);
            if (!result.has_value()) {
                std::cerr << "Error: " << result.error() << "\n";
                return 1;
            }
            std::cout << "Set " << output << " to " << w << "x" << h;
            if (rate > 0) std::cout << " @ " << rate << " Hz";
            std::cout << "\n";
        }

        if (!pos_str.empty()) {
            int x = 0, y = 0;
            if (sscanf(pos_str.c_str(), "%d,%d", &x, &y) != 2) {
                std::cerr << "Error: invalid position format, expected X,Y\n";
                return 1;
            }
            auto result = mgr.set_position(output, x, y);
            if (!result.has_value()) {
                std::cerr << "Error: " << result.error() << "\n";
                return 1;
            }
            std::cout << "Set " << output << " position to " << x << "," << y << "\n";
        }

        return 0;
    }

    // -----------------------------------------------------------------------
    // profile <save|load|list|delete>
    // -----------------------------------------------------------------------
    if (command == "profile") {
        if (argc < 3) {
            std::cerr << "Error: 'profile' requires a subcommand (save/load/list/delete)\n";
            return 1;
        }
        std::string sub = argv[2];

        if (sub == "save") {
            if (argc < 4) {
                std::cerr << "Error: 'profile save' requires a name\n";
                return 1;
            }
            auto res = mgr.save_profile(argv[3]);
            if (!res.has_value()) {
                std::cerr << "Error: " << res.error() << "\n";
                return 1;
            }
            std::cout << "Profile '" << argv[3] << "' saved.\n";
            return 0;
        }

        if (sub == "load") {
            if (argc < 4) {
                std::cerr << "Error: 'profile load' requires a name\n";
                return 1;
            }
            auto res = mgr.load_profile(argv[3]);
            if (!res.has_value()) {
                std::cerr << "Error: " << res.error() << "\n";
                return 1;
            }
            std::cout << "Profile '" << argv[3] << "' applied.\n";
            return 0;
        }

        if (sub == "list") {
            auto names = mgr.list_profiles();
            if (names.empty()) {
                std::cout << "No saved profiles.\n";
                return 0;
            }
            std::cout << "Saved display profiles:\n";
            for (const auto& n : names) {
                std::cout << "  " << n << "\n";
            }
            return 0;
        }

        if (sub == "delete") {
            if (argc < 4) {
                std::cerr << "Error: 'profile delete' requires a name\n";
                return 1;
            }
            auto res = mgr.delete_profile(argv[3]);
            if (!res.has_value()) {
                std::cerr << "Error: " << res.error() << "\n";
                return 1;
            }
            std::cout << "Profile '" << argv[3] << "' deleted.\n";
            return 0;
        }

        std::cerr << "Error: unknown profile subcommand '" << sub << "'\n";
        return 1;
    }

    // -----------------------------------------------------------------------
    // mirror <source> <dest>
    // -----------------------------------------------------------------------
    if (command == "mirror") {
        if (argc < 4) {
            std::cerr << "Error: 'mirror' requires source and destination outputs\n";
            return 1;
        }
        auto res = mgr.mirror(argv[2], argv[3]);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Mirroring " << argv[2] << " → " << argv[3] << "\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // rotate <output> <degrees>
    // -----------------------------------------------------------------------
    if (command == "rotate") {
        if (argc < 4) {
            std::cerr << "Error: 'rotate' requires output name and degrees\n";
            return 1;
        }
        int deg = std::atoi(argv[3]);
        auto res = mgr.rotate(argv[2], deg);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Rotated " << argv[2] << " to " << deg << " degrees.\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // night [--enable|--disable] [--temp=K] [--schedule=S]
    // -----------------------------------------------------------------------
    if (command == "night") {
        auto settings = mgr.get_night_mode();

        if (argc == 2) {
            // Show current settings
            std::cout << "Night mode: " << (settings.enabled ? "enabled" : "disabled") << "\n"
                      << "  Temperature: " << settings.temperature_k << " K\n";
            if (!settings.schedule.empty()) {
                std::cout << "  Schedule:    " << settings.schedule << "\n";
            }
            return 0;
        }

        if (has_flag(argc, argv, "--enable")) settings.enabled = true;
        if (has_flag(argc, argv, "--disable")) settings.enabled = false;

        std::string temp_str = get_arg(argc, argv, "--temp=");
        if (!temp_str.empty()) {
            settings.temperature_k = std::stoi(temp_str);
        }

        std::string sched_str = get_arg(argc, argv, "--schedule=");
        if (!sched_str.empty()) {
            settings.schedule = sched_str;
        }

        auto res = mgr.set_night_mode(settings);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Night mode " << (settings.enabled ? "enabled" : "disabled")
                  << " (" << settings.temperature_k << " K)\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // icc <output> <profile.icc>
    // -----------------------------------------------------------------------
    if (command == "icc") {
        if (argc < 4) {
            std::cerr << "Error: 'icc' requires output name and ICC profile path\n";
            return 1;
        }
        auto res = mgr.load_icc_profile(argv[2], argv[3]);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "ICC profile loaded for " << argv[2] << "\n";
        return 0;
    }

    std::cerr << "Error: unknown command '" << command << "'\n";
    print_usage();
    return 1;
}
