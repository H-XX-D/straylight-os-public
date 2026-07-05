// tools/input/main.cpp
// CLI front-end for straylight-input — input device configuration.

#include "input_manager.h"

#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>

static void print_usage() {
    std::cerr
        << "straylight-input — input device configurator\n"
        << "\n"
        << "Usage:\n"
        << "  straylight-input list                                           List input devices\n"
        << "  straylight-input set-keyboard --layout=us [--variant=dvorak] [--model=pc105]\n"
        << "                   [--options=ctrl:nocaps] [--repeat-delay=250] [--repeat-rate=33]\n"
        << "  straylight-input set-mouse <device> --speed=N [--accel=flat|adaptive]\n"
        << "                   [--natural-scroll] [--left-handed] [--dpi=N]\n"
        << "  straylight-input set-touchpad <device> [--tap] [--natural-scroll]\n"
        << "                   [--speed=N] [--disable-while-typing] [--click=areas|finger]\n"
        << "                   [--scroll=two-finger|edge|none] [--drag] [--drag-lock]\n"
        << "  straylight-input remap <device> <from-code> <to-name>           Remap a key\n"
        << "  straylight-input remap list                                     List remaps\n"
        << "  straylight-input remap remove <device> <from-code>              Remove remap\n"
        << "  straylight-input gamepad <device> [--deadzone=N] [--swap-ab] [--invert-y]\n"
        << "  straylight-input profile save <device>                          Save device profile\n"
        << "  straylight-input profile load <device>                          Load device profile\n"
        << "  straylight-input profile list                                   List profiles\n";
}

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

static const char* type_name(straylight::InputDevice::Type t) {
    switch (t) {
        case straylight::InputDevice::Type::Keyboard: return "Keyboard";
        case straylight::InputDevice::Type::Mouse:    return "Mouse";
        case straylight::InputDevice::Type::Touchpad: return "Touchpad";
        case straylight::InputDevice::Type::Gamepad:  return "Gamepad";
        case straylight::InputDevice::Type::Tablet:   return "Tablet";
        case straylight::InputDevice::Type::Other:    return "Other";
    }
    return "Unknown";
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

    straylight::InputManager mgr;

    // -----------------------------------------------------------------------
    // list
    // -----------------------------------------------------------------------
    if (command == "list") {
        auto res = mgr.list_devices();
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }

        const auto& devices = res.value();
        if (devices.empty()) {
            std::cout << "No input devices found.\n";
            return 0;
        }

        std::cout << std::left
                  << std::setw(24) << "DEVICE"
                  << std::setw(12) << "TYPE"
                  << std::setw(40) << "NAME"
                  << "CAPABILITIES\n";
        std::cout << std::string(90, '-') << "\n";

        for (const auto& dev : devices) {
            std::string caps;
            if (dev.has_keys) caps += "keys ";
            if (dev.has_rel_axes) caps += "rel ";
            if (dev.has_abs_axes) caps += "abs ";
            if (dev.has_force_feedback) caps += "ff ";

            std::cout << std::left
                      << std::setw(24) << dev.path
                      << std::setw(12) << type_name(dev.type)
                      << std::setw(40) << dev.name
                      << caps << "\n";
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // set-keyboard
    // -----------------------------------------------------------------------
    if (command == "set-keyboard") {
        straylight::KeyboardConfig config = mgr.get_keyboard();

        std::string layout = get_arg(argc, argv, "--layout=");
        std::string variant = get_arg(argc, argv, "--variant=");
        std::string model = get_arg(argc, argv, "--model=");
        std::string options = get_arg(argc, argv, "--options=");
        std::string delay_str = get_arg(argc, argv, "--repeat-delay=");
        std::string rate_str = get_arg(argc, argv, "--repeat-rate=");

        if (!layout.empty()) config.layout = layout;
        if (!variant.empty()) config.variant = variant;
        if (!model.empty()) config.model = model;
        if (!options.empty()) config.options = options;
        if (!delay_str.empty()) config.repeat_delay_ms = std::atoi(delay_str.c_str());
        if (!rate_str.empty()) config.repeat_rate_hz = std::atoi(rate_str.c_str());

        auto res = mgr.set_keyboard(config);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }

        std::cout << "Keyboard configured:\n"
                  << "  Layout:  " << config.layout;
        if (!config.variant.empty()) std::cout << " (" << config.variant << ")";
        std::cout << "\n";
        if (!config.model.empty()) std::cout << "  Model:   " << config.model << "\n";
        if (!config.options.empty()) std::cout << "  Options: " << config.options << "\n";
        std::cout << "  Repeat:  delay=" << config.repeat_delay_ms
                  << "ms rate=" << config.repeat_rate_hz << "Hz\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // set-mouse <device> [options]
    // -----------------------------------------------------------------------
    if (command == "set-mouse") {
        if (argc < 3) {
            std::cerr << "Error: 'set-mouse' requires a device name or path\n";
            return 1;
        }
        std::string device = argv[2];
        straylight::MouseConfig config;

        std::string speed_str = get_arg(argc, argv, "--speed=", 3);
        std::string accel_str = get_arg(argc, argv, "--accel=", 3);
        std::string dpi_str = get_arg(argc, argv, "--dpi=", 3);

        if (!speed_str.empty()) config.speed = std::stod(speed_str);
        if (accel_str == "flat") config.accel = straylight::MouseConfig::AccelProfile::Flat;
        else if (accel_str == "adaptive") config.accel = straylight::MouseConfig::AccelProfile::Adaptive;
        config.natural_scroll = has_flag(argc, argv, "--natural-scroll", 3);
        config.left_handed = has_flag(argc, argv, "--left-handed", 3);
        if (!dpi_str.empty()) config.dpi = std::atoi(dpi_str.c_str());

        auto res = mgr.set_mouse(device, config);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }

        std::cout << "Mouse '" << device << "' configured:\n"
                  << "  Speed: " << config.speed << "\n"
                  << "  Accel: " << (config.accel == straylight::MouseConfig::AccelProfile::Flat ? "flat" : "adaptive") << "\n"
                  << "  Natural scroll: " << (config.natural_scroll ? "yes" : "no") << "\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // set-touchpad <device> [options]
    // -----------------------------------------------------------------------
    if (command == "set-touchpad") {
        if (argc < 3) {
            std::cerr << "Error: 'set-touchpad' requires a device name or path\n";
            return 1;
        }
        std::string device = argv[2];
        straylight::TouchpadConfig config;

        std::string speed_str = get_arg(argc, argv, "--speed=", 3);
        std::string click_str = get_arg(argc, argv, "--click=", 3);
        std::string scroll_str = get_arg(argc, argv, "--scroll=", 3);

        config.tap_to_click = has_flag(argc, argv, "--tap", 3);
        config.natural_scroll = has_flag(argc, argv, "--natural-scroll", 3);
        config.disable_while_typing = has_flag(argc, argv, "--disable-while-typing", 3);
        config.drag = has_flag(argc, argv, "--drag", 3);
        config.drag_lock = has_flag(argc, argv, "--drag-lock", 3);

        if (!speed_str.empty()) config.speed = std::stod(speed_str);
        if (click_str == "areas") config.click_method = straylight::TouchpadConfig::ClickMethod::ButtonAreas;
        else if (click_str == "finger") config.click_method = straylight::TouchpadConfig::ClickMethod::Clickfinger;
        if (scroll_str == "two-finger") config.scroll_method = straylight::TouchpadConfig::ScrollMethod::TwoFinger;
        else if (scroll_str == "edge") config.scroll_method = straylight::TouchpadConfig::ScrollMethod::Edge;
        else if (scroll_str == "none") config.scroll_method = straylight::TouchpadConfig::ScrollMethod::None;

        auto res = mgr.set_touchpad(device, config);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }

        std::cout << "Touchpad '" << device << "' configured.\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // remap <device> <from-code> <to-name>  |  remap list  |  remap remove
    // -----------------------------------------------------------------------
    if (command == "remap") {
        if (argc < 3) {
            std::cerr << "Error: 'remap' requires arguments\n";
            return 1;
        }

        std::string sub = argv[2];

        if (sub == "list") {
            auto remaps = mgr.list_remaps();
            if (remaps.empty()) {
                std::cout << "No key remappings configured.\n";
                return 0;
            }
            std::cout << std::left
                      << std::setw(30) << "DEVICE"
                      << std::setw(12) << "FROM"
                      << std::setw(12) << "TO"
                      << "\n";
            std::cout << std::string(54, '-') << "\n";
            for (const auto& r : remaps) {
                std::cout << std::left
                          << std::setw(30) << r.device_pattern
                          << std::setw(12) << (r.from_name.empty() ? std::to_string(r.from_code) : r.from_name)
                          << std::setw(12) << r.to_name
                          << "\n";
            }
            return 0;
        }

        if (sub == "remove") {
            if (argc < 5) {
                std::cerr << "Error: 'remap remove' requires device and from-code\n";
                return 1;
            }
            auto res = mgr.remove_remap(argv[3], std::atoi(argv[4]));
            if (!res.has_value()) {
                std::cerr << "Error: " << res.error() << "\n";
                return 1;
            }
            std::cout << "Remap removed.\n";
            return 0;
        }

        // remap <device> <from-code> <to-name>
        if (argc < 5) {
            std::cerr << "Error: 'remap' requires: <device> <from-code> <to-name>\n";
            return 1;
        }

        straylight::KeyRemap remap;
        remap.device_pattern = argv[2];
        remap.from_code = std::atoi(argv[3]);
        remap.to_name = argv[4];
        remap.from_name = std::to_string(remap.from_code);

        auto res = mgr.add_remap(remap);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Key " << remap.from_code << " remapped to '" << remap.to_name
                  << "' on '" << remap.device_pattern << "'\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // gamepad <device> [--deadzone=N] [--swap-ab] [--invert-y]
    // -----------------------------------------------------------------------
    if (command == "gamepad") {
        if (argc < 3) {
            std::cerr << "Error: 'gamepad' requires a device name or path\n";
            return 1;
        }
        std::string device = argv[2];
        straylight::GamepadConfig config;

        std::string dz_str = get_arg(argc, argv, "--deadzone=", 3);
        std::string tt_str = get_arg(argc, argv, "--trigger=", 3);

        if (!dz_str.empty()) config.deadzone = std::stod(dz_str);
        if (!tt_str.empty()) config.trigger_threshold = std::stod(tt_str);
        config.swap_ab = has_flag(argc, argv, "--swap-ab", 3);
        config.invert_y = has_flag(argc, argv, "--invert-y", 3);

        auto res = mgr.set_gamepad(device, config);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }

        std::cout << "Gamepad '" << device << "' configured:\n"
                  << "  Deadzone: " << config.deadzone << "\n"
                  << "  Swap A/B: " << (config.swap_ab ? "yes" : "no") << "\n"
                  << "  Invert Y: " << (config.invert_y ? "yes" : "no") << "\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // profile save/load/list
    // -----------------------------------------------------------------------
    if (command == "profile") {
        if (argc < 3) {
            std::cerr << "Error: 'profile' requires a subcommand\n";
            return 1;
        }
        std::string sub = argv[2];

        if (sub == "list") {
            auto names = mgr.list_profiles();
            if (names.empty()) {
                std::cout << "No saved input profiles.\n";
                return 0;
            }
            std::cout << "Saved input profiles:\n";
            for (const auto& n : names) {
                std::cout << "  " << n << "\n";
            }
            return 0;
        }

        if (sub == "save" || sub == "load") {
            if (argc < 4) {
                std::cerr << "Error: 'profile " << sub << "' requires a device name\n";
                return 1;
            }
            auto res = (sub == "save") ? mgr.save_profile(argv[3]) : mgr.load_profile(argv[3]);
            if (!res.has_value()) {
                std::cerr << "Error: " << res.error() << "\n";
                return 1;
            }
            std::cout << "Profile " << sub << "ed for '" << argv[3] << "'.\n";
            return 0;
        }

        std::cerr << "Error: unknown profile subcommand '" << sub << "'\n";
        return 1;
    }

    std::cerr << "Error: unknown command '" << command << "'\n";
    print_usage();
    return 1;
}
