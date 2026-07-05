// tools/input/input_manager.h
// Input device configuration for StrayLight OS.
#pragma once

#include <straylight/result.h>

#include <cstdint>
#include <string>
#include <vector>

namespace straylight {

/// Describes an input device detected via evdev.
struct InputDevice {
    std::string path;          // /dev/input/event*
    std::string name;
    std::string phys;          // physical path (USB port, etc.)
    std::string sysfs_path;   // /sys/class/input/...
    uint16_t vendor_id = 0;
    uint16_t product_id = 0;

    enum class Type { Keyboard, Mouse, Touchpad, Gamepad, Tablet, Other };
    Type type = Type::Other;

    bool has_keys = false;
    bool has_rel_axes = false;
    bool has_abs_axes = false;
    bool has_force_feedback = false;
};

/// Keyboard layout configuration.
struct KeyboardConfig {
    std::string layout;        // e.g. "us", "de", "fr"
    std::string variant;       // e.g. "dvorak", "colemak"
    std::string model;         // e.g. "pc105"
    std::string options;       // e.g. "ctrl:nocaps"
    int repeat_delay_ms = 250;
    int repeat_rate_hz = 33;
};

/// Mouse / pointer configuration.
struct MouseConfig {
    double speed = 0.0;        // -1.0 to 1.0 (libinput pointer speed)
    enum class AccelProfile { Flat, Adaptive };
    AccelProfile accel = AccelProfile::Adaptive;
    bool natural_scroll = false;
    bool left_handed = false;
    int dpi = 0;               // 0 = device default
};

/// Touchpad configuration.
struct TouchpadConfig {
    bool tap_to_click = true;
    bool natural_scroll = true;
    bool disable_while_typing = true;
    double speed = 0.0;
    enum class ClickMethod { ButtonAreas, Clickfinger };
    ClickMethod click_method = ClickMethod::Clickfinger;
    enum class ScrollMethod { TwoFinger, Edge, None };
    ScrollMethod scroll_method = ScrollMethod::TwoFinger;
    bool drag = true;
    bool drag_lock = false;
    bool middle_emulation = false;
};

/// Gamepad calibration.
struct GamepadConfig {
    double deadzone = 0.1;     // 0.0–1.0
    bool swap_ab = false;
    bool invert_y = false;
    double trigger_threshold = 0.1;
};

/// Key remapping rule.
struct KeyRemap {
    std::string device_pattern;  // match by name or path glob
    int from_code = 0;
    int to_code = 0;
    std::string from_name;
    std::string to_name;
};

class InputManager {
public:
    InputManager();
    ~InputManager();

    /// Enumerate all input devices.
    Result<std::vector<InputDevice>, std::string> list_devices() const;

    /// Get detailed info for a specific device.
    Result<InputDevice, std::string> get_device(const std::string& path_or_name) const;

    /// Set keyboard layout/variant/options via XKB.
    Result<void, std::string> set_keyboard(const KeyboardConfig& config);

    /// Get current keyboard configuration.
    KeyboardConfig get_keyboard() const;

    /// Set mouse/pointer configuration.
    Result<void, std::string> set_mouse(const std::string& device, const MouseConfig& config);

    /// Set touchpad configuration.
    Result<void, std::string> set_touchpad(const std::string& device, const TouchpadConfig& config);

    /// Configure gamepad deadzone and calibration.
    Result<void, std::string> set_gamepad(const std::string& device, const GamepadConfig& config);

    /// Add a key remapping rule.
    Result<void, std::string> add_remap(const KeyRemap& remap);

    /// Remove a remapping rule.
    Result<void, std::string> remove_remap(const std::string& device_pattern, int from_code);

    /// List active remapping rules.
    std::vector<KeyRemap> list_remaps() const;

    /// Save per-device profile to config directory.
    Result<void, std::string> save_profile(const std::string& device_name);

    /// Load per-device profile from config directory.
    Result<void, std::string> load_profile(const std::string& device_name);

    /// List saved device profiles.
    std::vector<std::string> list_profiles() const;

private:
    static constexpr const char* kConfigDir = "input";

    /// Get full config directory path, expanding $HOME.
    std::string config_dir() const;

    /// Run a shell command and capture output.
    Result<std::string, std::string> run_cmd(const std::string& cmd) const;

    /// Detect device type from evdev capabilities.
    InputDevice::Type classify_device(const std::string& sysfs_path) const;

    /// Write a libinput quirks file for device-specific settings.
    Result<void, std::string> write_libinput_quirk(const std::string& device_name,
                                                     const std::string& content) const;

    /// Write udev hwdb entry for key remapping.
    Result<void, std::string> write_hwdb_remap(const KeyRemap& remap) const;

    /// Reload udev hwdb after changes.
    Result<void, std::string> reload_hwdb() const;

    /// Read XKB config from system.
    KeyboardConfig read_xkb_config() const;

    /// Write device-specific JSON profile.
    Result<void, std::string> write_device_profile(const std::string& device_name,
                                                     const std::string& json) const;

    /// Read device-specific JSON profile.
    Result<std::string, std::string> read_device_profile(const std::string& device_name) const;
};

} // namespace straylight
