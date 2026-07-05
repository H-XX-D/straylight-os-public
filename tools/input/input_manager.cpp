// tools/input/input_manager.cpp
// Full implementation of input device configuration for StrayLight OS.

#include "input_manager.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <linux/input.h>
#include <regex>
#include <sstream>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace straylight {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

InputManager::InputManager() {
    fs::create_directories(config_dir());
}

InputManager::~InputManager() = default;

std::string InputManager::config_dir() const {
    const char* home = std::getenv("HOME");
    if (!home) home = "/root";
    return std::string(home) + "/.config/straylight/" + kConfigDir;
}

Result<std::string, std::string> InputManager::run_cmd(const std::string& cmd) const {
    std::array<char, 4096> buffer{};
    std::string output;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return Result<std::string, std::string>::error(
            "popen failed: " + std::string(strerror(errno)));
    }
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }
    int rc = pclose(pipe);
    if (rc != 0) {
        return Result<std::string, std::string>::error(
            "command failed (rc=" + std::to_string(rc) + "): " + cmd +
            "\noutput: " + output);
    }
    return Result<std::string, std::string>::ok(output);
}

// ---------------------------------------------------------------------------
// Device classification from capabilities
// ---------------------------------------------------------------------------

InputDevice::Type InputManager::classify_device(const std::string& sysfs_path) const {
    // Read capabilities bitmaps from sysfs
    auto read_cap = [&](const std::string& cap_name) -> uint64_t {
        std::ifstream f(sysfs_path + "/device/capabilities/" + cap_name);
        if (!f.is_open()) return 0;
        std::string hex;
        f >> hex;
        if (hex.empty()) return 0;
        return std::stoull(hex, nullptr, 16);
    };

    uint64_t ev = read_cap("ev");
    uint64_t key = read_cap("key");
    uint64_t rel = read_cap("rel");
    uint64_t abs = read_cap("abs");
    uint64_t ff = read_cap("ff");

    bool has_keys = (ev & (1 << EV_KEY)) != 0;
    bool has_rel = (ev & (1 << EV_REL)) != 0;
    bool has_abs = (ev & (1 << EV_ABS)) != 0;
    bool has_ff_bit = (ev & (1 << EV_FF)) != 0;

    // Gamepad: has absolute axes + keys but no rel axes, and has force feedback
    // or has BTN_GAMEPAD range keys
    if (has_abs && has_keys && (has_ff_bit || (key & 0x1FFF0000) != 0)) {
        return InputDevice::Type::Gamepad;
    }

    // Touchpad: has absolute axes with specific properties
    // Check for BTN_TOOL_FINGER
    if (has_abs && has_keys) {
        // Read the device name to help classify
        std::ifstream name_f(sysfs_path + "/device/name");
        std::string name;
        if (name_f.is_open()) std::getline(name_f, name);
        std::transform(name.begin(), name.end(), name.begin(), ::tolower);
        if (name.find("touchpad") != std::string::npos ||
            name.find("trackpad") != std::string::npos ||
            name.find("synaptics") != std::string::npos) {
            return InputDevice::Type::Touchpad;
        }
        // Tablet
        if (name.find("tablet") != std::string::npos ||
            name.find("wacom") != std::string::npos ||
            name.find("pen") != std::string::npos) {
            return InputDevice::Type::Tablet;
        }
    }

    // Mouse: has relative axes (REL_X, REL_Y) and button keys
    if (has_rel && has_keys) {
        return InputDevice::Type::Mouse;
    }

    // Keyboard: has keys but no axes
    if (has_keys && !has_rel && !has_abs) {
        return InputDevice::Type::Keyboard;
    }

    // If has many keys, assume keyboard
    if (has_keys) {
        return InputDevice::Type::Keyboard;
    }

    return InputDevice::Type::Other;
}

// ---------------------------------------------------------------------------
// list_devices
// ---------------------------------------------------------------------------

Result<std::vector<InputDevice>, std::string> InputManager::list_devices() const {
    std::vector<InputDevice> devices;
    std::string input_dir = "/sys/class/input";

    if (!fs::exists(input_dir)) {
        return Result<std::vector<InputDevice>, std::string>::error(
            "/sys/class/input not available");
    }

    for (const auto& entry : fs::directory_iterator(input_dir)) {
        std::string name = entry.path().filename().string();
        if (name.rfind("event", 0) != 0) continue;

        InputDevice dev;
        dev.sysfs_path = entry.path().string();
        dev.path = "/dev/input/" + name;

        // Read device name
        std::ifstream name_f(dev.sysfs_path + "/device/name");
        if (name_f.is_open()) {
            std::getline(name_f, dev.name);
        }

        // Read physical path
        std::ifstream phys_f(dev.sysfs_path + "/device/phys");
        if (phys_f.is_open()) {
            std::getline(phys_f, dev.phys);
        }

        // Read vendor/product IDs from uevent
        std::ifstream uevent(dev.sysfs_path + "/device/uevent");
        if (uevent.is_open()) {
            std::string line;
            while (std::getline(uevent, line)) {
                if (line.rfind("HID_ID=", 0) == 0) {
                    // Format: HID_ID=0003:VVVV:PPPP
                    unsigned int bus = 0, vid = 0, pid = 0;
                    if (sscanf(line.c_str(), "HID_ID=%x:%x:%x", &bus, &vid, &pid) >= 2) {
                        dev.vendor_id = static_cast<uint16_t>(vid);
                        dev.product_id = static_cast<uint16_t>(pid);
                    }
                }
            }
        }

        // Classify device type
        dev.type = classify_device(dev.sysfs_path);

        // Read capability flags
        auto read_cap_flag = [&](const std::string& cap) -> bool {
            std::ifstream f(dev.sysfs_path + "/device/capabilities/" + cap);
            if (!f.is_open()) return false;
            std::string hex;
            f >> hex;
            return !hex.empty() && hex != "0";
        };

        dev.has_keys = read_cap_flag("key");
        dev.has_rel_axes = read_cap_flag("rel");
        dev.has_abs_axes = read_cap_flag("abs");
        dev.has_force_feedback = read_cap_flag("ff");

        devices.push_back(dev);
    }

    // Sort by path for stable ordering
    std::sort(devices.begin(), devices.end(),
              [](const auto& a, const auto& b) { return a.path < b.path; });

    return Result<std::vector<InputDevice>, std::string>::ok(devices);
}

// ---------------------------------------------------------------------------
// get_device
// ---------------------------------------------------------------------------

Result<InputDevice, std::string> InputManager::get_device(const std::string& path_or_name) const {
    auto res = list_devices();
    if (!res.has_value()) {
        return Result<InputDevice, std::string>::error(res.error());
    }

    for (const auto& dev : res.value()) {
        if (dev.path == path_or_name || dev.name == path_or_name) {
            return Result<InputDevice, std::string>::ok(dev);
        }
    }
    return Result<InputDevice, std::string>::error(
        "device not found: " + path_or_name);
}

// ---------------------------------------------------------------------------
// Keyboard configuration
// ---------------------------------------------------------------------------

KeyboardConfig InputManager::read_xkb_config() const {
    KeyboardConfig config;

    // Try reading from localectl
    auto res = run_cmd("localectl status 2>/dev/null");
    if (res.has_value()) {
        std::istringstream stream(res.value());
        std::string line;
        while (std::getline(stream, line)) {
            // Trim
            auto pos = line.find_first_not_of(" \t");
            if (pos != std::string::npos) line = line.substr(pos);

            if (line.rfind("X11 Layout:", 0) == 0 || line.rfind("VC Keymap:", 0) == 0) {
                auto colon = line.find(':');
                if (colon != std::string::npos) {
                    auto val = line.substr(colon + 1);
                    auto vpos = val.find_first_not_of(" \t");
                    if (vpos != std::string::npos) val = val.substr(vpos);
                    if (line.find("Layout") != std::string::npos) config.layout = val;
                }
            }
            if (line.rfind("X11 Variant:", 0) == 0) {
                auto colon = line.find(':');
                if (colon != std::string::npos) {
                    auto val = line.substr(colon + 1);
                    auto vpos = val.find_first_not_of(" \t");
                    if (vpos != std::string::npos) config.variant = val.substr(0, val.find_last_not_of(" \t\n\r") + 1);
                }
            }
            if (line.rfind("X11 Model:", 0) == 0) {
                auto colon = line.find(':');
                if (colon != std::string::npos) {
                    auto val = line.substr(colon + 1);
                    auto vpos = val.find_first_not_of(" \t");
                    if (vpos != std::string::npos) config.model = val.substr(0, val.find_last_not_of(" \t\n\r") + 1);
                }
            }
            if (line.rfind("X11 Options:", 0) == 0) {
                auto colon = line.find(':');
                if (colon != std::string::npos) {
                    auto val = line.substr(colon + 1);
                    auto vpos = val.find_first_not_of(" \t");
                    if (vpos != std::string::npos) config.options = val.substr(0, val.find_last_not_of(" \t\n\r") + 1);
                }
            }
        }
    }

    // Read repeat rate from /sys
    std::ifstream delay_f("/sys/module/i8042/parameters/delay");
    if (delay_f.is_open()) {
        delay_f >> config.repeat_delay_ms;
    }
    std::ifstream rate_f("/sys/module/i8042/parameters/rate");
    if (rate_f.is_open()) {
        rate_f >> config.repeat_rate_hz;
    }

    return config;
}

Result<void, std::string> InputManager::set_keyboard(const KeyboardConfig& config) {
    // Build setxkbmap command
    std::ostringstream cmd;
    cmd << "setxkbmap";
    if (!config.layout.empty()) cmd << " -layout " << config.layout;
    if (!config.variant.empty()) cmd << " -variant " << config.variant;
    if (!config.model.empty()) cmd << " -model " << config.model;
    if (!config.options.empty()) cmd << " -option " << config.options;
    cmd << " 2>/dev/null";

    auto res = run_cmd(cmd.str());

    // Also try localectl for persistent configuration
    std::ostringstream lcmd;
    lcmd << "localectl set-x11-keymap";
    if (!config.layout.empty()) lcmd << " " << config.layout;
    else lcmd << " us";
    if (!config.model.empty()) lcmd << " " << config.model;
    if (!config.variant.empty()) lcmd << " " << config.variant;
    if (!config.options.empty()) lcmd << " " << config.options;
    lcmd << " 2>/dev/null";
    run_cmd(lcmd.str());

    // Set repeat rate
    if (config.repeat_delay_ms > 0 && config.repeat_rate_hz > 0) {
        // Calculate rate as interval in ms
        double interval_ms = 1000.0 / config.repeat_rate_hz;
        std::ostringstream rep_cmd;
        rep_cmd << "xset r rate " << config.repeat_delay_ms
                << " " << config.repeat_rate_hz << " 2>/dev/null";
        run_cmd(rep_cmd.str());
    }

    // Save config to profile
    std::ostringstream json;
    json << "{\n"
         << "  \"layout\": \"" << config.layout << "\",\n"
         << "  \"variant\": \"" << config.variant << "\",\n"
         << "  \"model\": \"" << config.model << "\",\n"
         << "  \"options\": \"" << config.options << "\",\n"
         << "  \"repeat_delay_ms\": " << config.repeat_delay_ms << ",\n"
         << "  \"repeat_rate_hz\": " << config.repeat_rate_hz << "\n"
         << "}\n";

    write_device_profile("keyboard", json.str());

    return Result<void, std::string>::ok();
}

KeyboardConfig InputManager::get_keyboard() const {
    // Try reading saved profile first
    auto profile = read_device_profile("keyboard");
    if (profile.has_value()) {
        KeyboardConfig config;
        std::string content = profile.value();

        std::regex layout_re(R"("layout"\s*:\s*"([^"]*)")");
        std::regex variant_re(R"("variant"\s*:\s*"([^"]*)")");
        std::regex model_re(R"("model"\s*:\s*"([^"]*)")");
        std::regex options_re(R"("options"\s*:\s*"([^"]*)")");
        std::regex delay_re(R"("repeat_delay_ms"\s*:\s*(\d+))");
        std::regex rate_re(R"("repeat_rate_hz"\s*:\s*(\d+))");

        std::smatch m;
        if (std::regex_search(content, m, layout_re)) config.layout = m[1].str();
        if (std::regex_search(content, m, variant_re)) config.variant = m[1].str();
        if (std::regex_search(content, m, model_re)) config.model = m[1].str();
        if (std::regex_search(content, m, options_re)) config.options = m[1].str();
        if (std::regex_search(content, m, delay_re)) config.repeat_delay_ms = std::stoi(m[1].str());
        if (std::regex_search(content, m, rate_re)) config.repeat_rate_hz = std::stoi(m[1].str());

        return config;
    }

    return read_xkb_config();
}

// ---------------------------------------------------------------------------
// Mouse configuration
// ---------------------------------------------------------------------------

Result<void, std::string> InputManager::set_mouse(const std::string& device,
                                                    const MouseConfig& config) {
    // Find device by name or path
    auto dev_res = get_device(device);
    std::string device_name = dev_res.has_value() ? dev_res.value().name : device;

    // Write libinput config via xinput for X11, or sway/hyprland config for Wayland
    std::string accel_profile = (config.accel == MouseConfig::AccelProfile::Flat) ? "flat" : "adaptive";

    // Try xinput
    std::ostringstream cmd;
    cmd << "xinput set-prop '" << device_name << "' 'libinput Accel Speed' "
        << config.speed << " 2>/dev/null";
    run_cmd(cmd.str());

    cmd.str("");
    cmd << "xinput set-prop '" << device_name << "' 'libinput Accel Profile Enabled' "
        << (config.accel == MouseConfig::AccelProfile::Flat ? "1 0" : "0 1")
        << " 2>/dev/null";
    run_cmd(cmd.str());

    cmd.str("");
    cmd << "xinput set-prop '" << device_name << "' 'libinput Natural Scrolling Enabled' "
        << (config.natural_scroll ? "1" : "0") << " 2>/dev/null";
    run_cmd(cmd.str());

    cmd.str("");
    cmd << "xinput set-prop '" << device_name << "' 'libinput Left Handed Enabled' "
        << (config.left_handed ? "1" : "0") << " 2>/dev/null";
    run_cmd(cmd.str());

    // Write libinput quirks file for persistent config
    std::ostringstream quirks;
    quirks << "[" << device_name << "]\n"
           << "MatchName=" << device_name << "\n"
           << "AttrAccelProfile=" << accel_profile << "\n"
           << "AttrAccelSpeed=" << config.speed << "\n";

    write_libinput_quirk(device_name, quirks.str());

    // Save device profile
    std::ostringstream json;
    json << "{\n"
         << "  \"type\": \"mouse\",\n"
         << "  \"speed\": " << config.speed << ",\n"
         << "  \"accel_profile\": \"" << accel_profile << "\",\n"
         << "  \"natural_scroll\": " << (config.natural_scroll ? "true" : "false") << ",\n"
         << "  \"left_handed\": " << (config.left_handed ? "true" : "false") << ",\n"
         << "  \"dpi\": " << config.dpi << "\n"
         << "}\n";
    write_device_profile(device_name, json.str());

    return Result<void, std::string>::ok();
}

// ---------------------------------------------------------------------------
// Touchpad configuration
// ---------------------------------------------------------------------------

Result<void, std::string> InputManager::set_touchpad(const std::string& device,
                                                       const TouchpadConfig& config) {
    auto dev_res = get_device(device);
    std::string device_name = dev_res.has_value() ? dev_res.value().name : device;

    // Apply via xinput
    auto set_prop = [&](const std::string& prop, const std::string& val) {
        std::string cmd = "xinput set-prop '" + device_name + "' '" + prop + "' " + val + " 2>/dev/null";
        run_cmd(cmd);
    };

    set_prop("libinput Tapping Enabled", config.tap_to_click ? "1" : "0");
    set_prop("libinput Natural Scrolling Enabled", config.natural_scroll ? "1" : "0");
    set_prop("libinput Disable While Typing Enabled", config.disable_while_typing ? "1" : "0");
    set_prop("libinput Accel Speed", std::to_string(config.speed));

    std::string click_method = (config.click_method == TouchpadConfig::ClickMethod::Clickfinger)
                                   ? "0 1" : "1 0";
    set_prop("libinput Click Method Enabled", click_method);

    std::string scroll_method;
    switch (config.scroll_method) {
        case TouchpadConfig::ScrollMethod::TwoFinger: scroll_method = "1 0 0"; break;
        case TouchpadConfig::ScrollMethod::Edge:      scroll_method = "0 1 0"; break;
        case TouchpadConfig::ScrollMethod::None:      scroll_method = "0 0 0"; break;
    }
    set_prop("libinput Scroll Method Enabled", scroll_method);
    set_prop("libinput Tapping Drag Enabled", config.drag ? "1" : "0");
    set_prop("libinput Tapping Drag Lock Enabled", config.drag_lock ? "1" : "0");
    set_prop("libinput Middle Emulation Enabled", config.middle_emulation ? "1" : "0");

    // Write libinput quirks for persistence
    std::string click_m = (config.click_method == TouchpadConfig::ClickMethod::Clickfinger)
                              ? "clickfinger" : "button-areas";
    std::string scroll_m;
    switch (config.scroll_method) {
        case TouchpadConfig::ScrollMethod::TwoFinger: scroll_m = "two-finger"; break;
        case TouchpadConfig::ScrollMethod::Edge:      scroll_m = "edge"; break;
        case TouchpadConfig::ScrollMethod::None:      scroll_m = "none"; break;
    }

    std::ostringstream quirks;
    quirks << "[" << device_name << "]\n"
           << "MatchName=" << device_name << "\n"
           << "AttrTapEnabled=true\n"
           << "AttrClickMethod=" << click_m << "\n"
           << "AttrScrollMethod=" << scroll_m << "\n";
    write_libinput_quirk(device_name, quirks.str());

    // Save profile JSON
    std::ostringstream json;
    json << "{\n"
         << "  \"type\": \"touchpad\",\n"
         << "  \"tap_to_click\": " << (config.tap_to_click ? "true" : "false") << ",\n"
         << "  \"natural_scroll\": " << (config.natural_scroll ? "true" : "false") << ",\n"
         << "  \"disable_while_typing\": " << (config.disable_while_typing ? "true" : "false") << ",\n"
         << "  \"speed\": " << config.speed << ",\n"
         << "  \"click_method\": \"" << click_m << "\",\n"
         << "  \"scroll_method\": \"" << scroll_m << "\",\n"
         << "  \"drag\": " << (config.drag ? "true" : "false") << ",\n"
         << "  \"drag_lock\": " << (config.drag_lock ? "true" : "false") << ",\n"
         << "  \"middle_emulation\": " << (config.middle_emulation ? "true" : "false") << "\n"
         << "}\n";
    write_device_profile(device_name, json.str());

    return Result<void, std::string>::ok();
}

// ---------------------------------------------------------------------------
// Gamepad configuration
// ---------------------------------------------------------------------------

Result<void, std::string> InputManager::set_gamepad(const std::string& device,
                                                      const GamepadConfig& config) {
    auto dev_res = get_device(device);
    if (!dev_res.has_value()) {
        return Result<void, std::string>::error("device not found: " + device);
    }

    const auto& dev = dev_res.value();
    std::string device_name = dev.name;

    // Use evdev-joystick for deadzone calibration
    int deadzone_raw = static_cast<int>(config.deadzone * 32767);

    // Set deadzone for each absolute axis
    for (int axis = ABS_X; axis <= ABS_RZ; ++axis) {
        std::ostringstream cmd;
        cmd << "evdev-joystick --e " << dev.path
            << " --a " << axis
            << " --deadzone " << deadzone_raw
            << " 2>/dev/null";
        run_cmd(cmd.str());
    }

    // Save profile
    std::ostringstream json;
    json << "{\n"
         << "  \"type\": \"gamepad\",\n"
         << "  \"deadzone\": " << config.deadzone << ",\n"
         << "  \"swap_ab\": " << (config.swap_ab ? "true" : "false") << ",\n"
         << "  \"invert_y\": " << (config.invert_y ? "true" : "false") << ",\n"
         << "  \"trigger_threshold\": " << config.trigger_threshold << "\n"
         << "}\n";
    write_device_profile(device_name, json.str());

    return Result<void, std::string>::ok();
}

// ---------------------------------------------------------------------------
// Key remapping
// ---------------------------------------------------------------------------

Result<void, std::string> InputManager::write_hwdb_remap(const KeyRemap& remap) const {
    // Write to /etc/udev/hwdb.d/90-straylight-remap.hwdb
    std::string hwdb_path = "/etc/udev/hwdb.d/90-straylight-remap.hwdb";

    // Read existing content
    std::string content;
    {
        std::ifstream f(hwdb_path);
        if (f.is_open()) {
            content = std::string((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
        }
    }

    // Build the remap entry
    // Format:
    // evdev:name:DeviceName:*
    //  KEYBOARD_KEY_<scancode>=<keyname>
    std::ostringstream entry;
    entry << "\nevdev:name:" << remap.device_pattern << ":*\n"
          << " KEYBOARD_KEY_" << std::hex << remap.from_code
          << "=" << remap.to_name << "\n";

    content += entry.str();

    std::ofstream out(hwdb_path);
    if (!out.is_open()) {
        return Result<void, std::string>::error(
            "cannot write to " + hwdb_path + " (run as root?)");
    }
    out << content;
    out.close();

    return reload_hwdb();
}

Result<void, std::string> InputManager::reload_hwdb() const {
    auto res = run_cmd("systemd-hwdb update 2>/dev/null && udevadm trigger 2>/dev/null");
    if (!res.has_value()) {
        return Result<void, std::string>::error("failed to reload hwdb: " + res.error());
    }
    return Result<void, std::string>::ok();
}

Result<void, std::string> InputManager::add_remap(const KeyRemap& remap) {
    // Save to our config for tracking
    std::string remaps_path = config_dir() + "/remaps.json";

    std::string content;
    {
        std::ifstream f(remaps_path);
        if (f.is_open()) {
            content = std::string((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
        }
    }

    if (content.empty()) {
        content = "{ \"remaps\": [] }";
    }

    // Build remap JSON entry
    std::ostringstream json_entry;
    json_entry << "{ \"device\": \"" << remap.device_pattern << "\","
               << " \"from_code\": " << remap.from_code << ","
               << " \"to_code\": " << remap.to_code << ","
               << " \"from_name\": \"" << remap.from_name << "\","
               << " \"to_name\": \"" << remap.to_name << "\" }";

    // Insert before closing bracket
    auto arr_end = content.rfind(']');
    if (arr_end == std::string::npos) {
        return Result<void, std::string>::error("malformed remaps config");
    }

    bool has_entries = (content.find('{', content.find('[')) != arr_end &&
                        content.find('{', content.find('[')) != std::string::npos);
    std::string insert = (has_entries ? ", " : "") + json_entry.str();
    content.insert(arr_end, insert);

    std::ofstream out(remaps_path);
    if (!out.is_open()) {
        return Result<void, std::string>::error("cannot write remaps config");
    }
    out << content;
    out.close();

    // Apply via hwdb
    return write_hwdb_remap(remap);
}

Result<void, std::string> InputManager::remove_remap(const std::string& device_pattern,
                                                       int from_code) {
    std::string remaps_path = config_dir() + "/remaps.json";
    std::ifstream f(remaps_path);
    if (!f.is_open()) {
        return Result<void, std::string>::error("no remaps configured");
    }

    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    f.close();

    // Find and remove the matching entry
    std::string search = "\"device\": \"" + device_pattern + "\"";
    auto pos = content.find(search);
    while (pos != std::string::npos) {
        // Check if this entry also matches from_code
        auto obj_start = content.rfind('{', pos);
        int depth = 1;
        auto obj_end = obj_start + 1;
        while (obj_end < content.size() && depth > 0) {
            if (content[obj_end] == '{') ++depth;
            else if (content[obj_end] == '}') --depth;
            ++obj_end;
        }

        std::string obj = content.substr(obj_start, obj_end - obj_start);
        std::string code_search = "\"from_code\": " + std::to_string(from_code);
        if (obj.find(code_search) != std::string::npos) {
            // Remove this entry plus any trailing comma
            auto erase_end = obj_end;
            while (erase_end < content.size() &&
                   (content[erase_end] == ' ' || content[erase_end] == ',')) ++erase_end;
            content.erase(obj_start, erase_end - obj_start);
            break;
        }

        pos = content.find(search, pos + 1);
    }

    std::ofstream out(remaps_path);
    out << content;

    // Rebuild hwdb from remaining remaps
    auto remaps = list_remaps();

    // Clear and rewrite hwdb file
    std::string hwdb_path = "/etc/udev/hwdb.d/90-straylight-remap.hwdb";
    std::ofstream hwdb(hwdb_path);
    if (hwdb.is_open()) {
        hwdb << "# StrayLight OS key remapping rules\n";
        for (const auto& r : remaps) {
            hwdb << "\nevdev:name:" << r.device_pattern << ":*\n"
                 << " KEYBOARD_KEY_" << std::hex << r.from_code
                 << "=" << r.to_name << "\n";
        }
    }

    return reload_hwdb();
}

std::vector<KeyRemap> InputManager::list_remaps() const {
    std::vector<KeyRemap> remaps;
    std::string remaps_path = config_dir() + "/remaps.json";
    std::ifstream f(remaps_path);
    if (!f.is_open()) return remaps;

    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());

    // Parse JSON array entries
    auto arr_start = content.find('[');
    auto arr_end = content.rfind(']');
    if (arr_start == std::string::npos || arr_end == std::string::npos) return remaps;

    std::string arr = content.substr(arr_start + 1, arr_end - arr_start - 1);
    size_t pos = 0;

    while (true) {
        auto obj_start = arr.find('{', pos);
        if (obj_start == std::string::npos) break;

        int depth = 1;
        auto obj_end = obj_start + 1;
        while (obj_end < arr.size() && depth > 0) {
            if (arr[obj_end] == '{') ++depth;
            else if (arr[obj_end] == '}') --depth;
            ++obj_end;
        }

        std::string entry = arr.substr(obj_start, obj_end - obj_start);

        KeyRemap remap;
        std::regex dev_re(R"("device"\s*:\s*"([^"]*)")");
        std::regex fc_re(R"("from_code"\s*:\s*(\d+))");
        std::regex tc_re(R"("to_code"\s*:\s*(\d+))");
        std::regex fn_re(R"("from_name"\s*:\s*"([^"]*)")");
        std::regex tn_re(R"("to_name"\s*:\s*"([^"]*)")");

        std::smatch m;
        if (std::regex_search(entry, m, dev_re)) remap.device_pattern = m[1].str();
        if (std::regex_search(entry, m, fc_re)) remap.from_code = std::stoi(m[1].str());
        if (std::regex_search(entry, m, tc_re)) remap.to_code = std::stoi(m[1].str());
        if (std::regex_search(entry, m, fn_re)) remap.from_name = m[1].str();
        if (std::regex_search(entry, m, tn_re)) remap.to_name = m[1].str();

        remaps.push_back(remap);
        pos = obj_end;
    }

    return remaps;
}

// ---------------------------------------------------------------------------
// Profile management
// ---------------------------------------------------------------------------

Result<void, std::string> InputManager::write_libinput_quirk(const std::string& device_name,
                                                               const std::string& content) const {
    // Sanitize device name for filename
    std::string safe_name = device_name;
    for (char& c : safe_name) {
        if (c == '/' || c == ' ' || c == '\'' || c == '"') c = '_';
    }

    std::string quirks_dir = "/etc/libinput/local-overrides.d";
    fs::create_directories(quirks_dir);

    std::string path = quirks_dir + "/straylight-" + safe_name + ".quirks";
    std::ofstream out(path);
    if (!out.is_open()) {
        // Fallback to user config
        std::string user_path = config_dir() + "/" + safe_name + ".quirks";
        out.open(user_path);
        if (!out.is_open()) {
            return Result<void, std::string>::error("cannot write quirks file");
        }
    }
    out << content;
    return Result<void, std::string>::ok();
}

Result<void, std::string> InputManager::write_device_profile(const std::string& device_name,
                                                               const std::string& json) const {
    std::string safe_name = device_name;
    for (char& c : safe_name) {
        if (c == '/' || c == ' ' || c == '\'' || c == '"') c = '_';
    }

    std::string path = config_dir() + "/" + safe_name + ".json";
    std::ofstream out(path);
    if (!out.is_open()) {
        return Result<void, std::string>::error("cannot write profile to " + path);
    }
    out << json;
    return Result<void, std::string>::ok();
}

Result<std::string, std::string> InputManager::read_device_profile(const std::string& device_name) const {
    std::string safe_name = device_name;
    for (char& c : safe_name) {
        if (c == '/' || c == ' ' || c == '\'' || c == '"') c = '_';
    }

    std::string path = config_dir() + "/" + safe_name + ".json";
    std::ifstream f(path);
    if (!f.is_open()) {
        return Result<std::string, std::string>::error("profile not found: " + path);
    }

    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    return Result<std::string, std::string>::ok(content);
}

Result<void, std::string> InputManager::save_profile(const std::string& device_name) {
    // Profile already saved by set_* methods; this forces a re-save of current state
    auto dev_res = get_device(device_name);
    if (!dev_res.has_value()) {
        return Result<void, std::string>::error(dev_res.error());
    }

    const auto& dev = dev_res.value();
    std::ostringstream json;
    json << "{\n"
         << "  \"name\": \"" << dev.name << "\",\n"
         << "  \"path\": \"" << dev.path << "\",\n"
         << "  \"vendor_id\": " << dev.vendor_id << ",\n"
         << "  \"product_id\": " << dev.product_id << ",\n"
         << "  \"type\": \"";
    switch (dev.type) {
        case InputDevice::Type::Keyboard: json << "keyboard"; break;
        case InputDevice::Type::Mouse:    json << "mouse"; break;
        case InputDevice::Type::Touchpad: json << "touchpad"; break;
        case InputDevice::Type::Gamepad:  json << "gamepad"; break;
        case InputDevice::Type::Tablet:   json << "tablet"; break;
        case InputDevice::Type::Other:    json << "other"; break;
    }
    json << "\"\n}\n";

    return write_device_profile(dev.name, json.str());
}

Result<void, std::string> InputManager::load_profile(const std::string& device_name) {
    auto profile = read_device_profile(device_name);
    if (!profile.has_value()) {
        return Result<void, std::string>::error(profile.error());
    }

    const std::string& content = profile.value();

    // Determine type and apply settings
    std::regex type_re(R"("type"\s*:\s*"([^"]*)")");
    std::smatch m;
    if (!std::regex_search(content, m, type_re)) {
        return Result<void, std::string>::error("profile missing type field");
    }

    std::string type = m[1].str();

    if (type == "mouse") {
        MouseConfig config;
        std::regex speed_re(R"("speed"\s*:\s*(-?\d+\.?\d*))");
        std::regex accel_re(R"("accel_profile"\s*:\s*"([^"]*)")");
        std::regex ns_re(R"("natural_scroll"\s*:\s*(true|false))");
        std::regex lh_re(R"("left_handed"\s*:\s*(true|false))");
        std::regex dpi_re(R"("dpi"\s*:\s*(\d+))");

        if (std::regex_search(content, m, speed_re)) config.speed = std::stod(m[1].str());
        if (std::regex_search(content, m, accel_re)) {
            config.accel = (m[1].str() == "flat") ? MouseConfig::AccelProfile::Flat
                                                   : MouseConfig::AccelProfile::Adaptive;
        }
        if (std::regex_search(content, m, ns_re)) config.natural_scroll = (m[1].str() == "true");
        if (std::regex_search(content, m, lh_re)) config.left_handed = (m[1].str() == "true");
        if (std::regex_search(content, m, dpi_re)) config.dpi = std::stoi(m[1].str());

        return set_mouse(device_name, config);
    }

    if (type == "touchpad") {
        TouchpadConfig config;
        std::regex tap_re(R"("tap_to_click"\s*:\s*(true|false))");
        std::regex ns_re(R"("natural_scroll"\s*:\s*(true|false))");
        std::regex dwt_re(R"("disable_while_typing"\s*:\s*(true|false))");
        std::regex speed_re(R"("speed"\s*:\s*(-?\d+\.?\d*))");
        std::regex drag_re(R"("drag"\s*:\s*(true|false))");
        std::regex dl_re(R"("drag_lock"\s*:\s*(true|false))");
        std::regex me_re(R"("middle_emulation"\s*:\s*(true|false))");

        if (std::regex_search(content, m, tap_re)) config.tap_to_click = (m[1].str() == "true");
        if (std::regex_search(content, m, ns_re)) config.natural_scroll = (m[1].str() == "true");
        if (std::regex_search(content, m, dwt_re)) config.disable_while_typing = (m[1].str() == "true");
        if (std::regex_search(content, m, speed_re)) config.speed = std::stod(m[1].str());
        if (std::regex_search(content, m, drag_re)) config.drag = (m[1].str() == "true");
        if (std::regex_search(content, m, dl_re)) config.drag_lock = (m[1].str() == "true");
        if (std::regex_search(content, m, me_re)) config.middle_emulation = (m[1].str() == "true");

        return set_touchpad(device_name, config);
    }

    if (type == "gamepad") {
        GamepadConfig config;
        std::regex dz_re(R"("deadzone"\s*:\s*(\d+\.?\d*))");
        std::regex swap_re(R"("swap_ab"\s*:\s*(true|false))");
        std::regex invert_re(R"("invert_y"\s*:\s*(true|false))");
        std::regex tt_re(R"("trigger_threshold"\s*:\s*(\d+\.?\d*))");

        if (std::regex_search(content, m, dz_re)) config.deadzone = std::stod(m[1].str());
        if (std::regex_search(content, m, swap_re)) config.swap_ab = (m[1].str() == "true");
        if (std::regex_search(content, m, invert_re)) config.invert_y = (m[1].str() == "true");
        if (std::regex_search(content, m, tt_re)) config.trigger_threshold = std::stod(m[1].str());

        return set_gamepad(device_name, config);
    }

    if (type == "keyboard") {
        KeyboardConfig config;
        std::regex layout_re(R"("layout"\s*:\s*"([^"]*)")");
        std::regex variant_re(R"("variant"\s*:\s*"([^"]*)")");
        std::regex model_re(R"("model"\s*:\s*"([^"]*)")");
        std::regex options_re(R"("options"\s*:\s*"([^"]*)")");

        if (std::regex_search(content, m, layout_re)) config.layout = m[1].str();
        if (std::regex_search(content, m, variant_re)) config.variant = m[1].str();
        if (std::regex_search(content, m, model_re)) config.model = m[1].str();
        if (std::regex_search(content, m, options_re)) config.options = m[1].str();

        return set_keyboard(config);
    }

    return Result<void, std::string>::error("unknown device type: " + type);
}

std::vector<std::string> InputManager::list_profiles() const {
    std::vector<std::string> names;
    std::string dir = config_dir();
    if (!fs::exists(dir)) return names;

    for (const auto& entry : fs::directory_iterator(dir)) {
        std::string fname = entry.path().filename().string();
        if (fname.size() > 5 && fname.substr(fname.size() - 5) == ".json" &&
            fname != "remaps.json") {
            names.push_back(fname.substr(0, fname.size() - 5));
        }
    }

    std::sort(names.begin(), names.end());
    return names;
}

} // namespace straylight
