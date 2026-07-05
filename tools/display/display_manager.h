// tools/display/display_manager.h
// Display/monitor configuration for StrayLight OS.
#pragma once

#include <straylight/result.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace straylight {

/// Describes a single display mode (resolution + refresh rate).
struct DisplayMode {
    int width = 0;
    int height = 0;
    double refresh_hz = 60.0;
    bool current = false;
    bool preferred = false;
};

/// Represents a physical output (monitor, projector, etc.).
struct OutputInfo {
    std::string name;         // e.g. "HDMI-A-1", "eDP-1"
    std::string make;
    std::string model;
    std::string serial;
    bool connected = false;
    bool enabled = false;
    int pos_x = 0;
    int pos_y = 0;
    int rotation_deg = 0;     // 0, 90, 180, 270
    DisplayMode active_mode;
    std::vector<DisplayMode> modes;
};

/// Multi-monitor layout profile.
struct DisplayProfile {
    std::string name;
    struct OutputConfig {
        std::string output_name;
        int width = 0;
        int height = 0;
        double refresh_hz = 60.0;
        int pos_x = 0;
        int pos_y = 0;
        int rotation_deg = 0;
        bool enabled = true;
        bool primary = false;
    };
    std::vector<OutputConfig> outputs;
};

/// Color temperature / night-mode settings.
struct NightModeSettings {
    bool enabled = false;
    int temperature_k = 4500;   // 1000–6500
    std::string schedule;       // "sunset-sunrise" or "HH:MM-HH:MM"
};

class DisplayManager {
public:
    DisplayManager();
    ~DisplayManager();

    /// Enumerate connected outputs from /sys/class/drm/card*/ and xrandr/wlr-randr.
    Result<std::vector<OutputInfo>, std::string> list_outputs() const;

    /// Set resolution and refresh rate for a specific output.
    Result<void, std::string> set_mode(const std::string& output, int width, int height,
                                        double refresh_hz);

    /// Set the position of an output in the multi-monitor layout.
    Result<void, std::string> set_position(const std::string& output, int x, int y);

    /// Rotate an output by the given degrees (0, 90, 180, 270).
    Result<void, std::string> rotate(const std::string& output, int degrees);

    /// Mirror source output onto destination output.
    Result<void, std::string> mirror(const std::string& source, const std::string& dest);

    /// Save the current layout as a named profile.
    Result<void, std::string> save_profile(const std::string& name);

    /// Load and apply a named profile.
    Result<void, std::string> load_profile(const std::string& name);

    /// List all saved display profiles.
    std::vector<std::string> list_profiles() const;

    /// Delete a saved profile.
    Result<void, std::string> delete_profile(const std::string& name);

    /// Set color temperature (night mode).
    Result<void, std::string> set_color_temp(int temperature_k);

    /// Configure night-mode schedule.
    Result<void, std::string> set_night_mode(const NightModeSettings& settings);

    /// Get current night-mode settings.
    NightModeSettings get_night_mode() const;

    /// Load an ICC color profile for an output.
    Result<void, std::string> load_icc_profile(const std::string& output,
                                                const std::string& icc_path);

private:
    static constexpr const char* kConfigDir = "/.config/straylight";
    static constexpr const char* kProfilesFile = "displays.json";

    /// Detect whether we're running under Wayland or X11.
    enum class Backend { Wayland, X11, DRM };
    Backend detect_backend() const;

    /// Run a shell command and capture output.
    Result<std::string, std::string> run_cmd(const std::string& cmd) const;

    /// Parse xrandr --query output into OutputInfo structs.
    std::vector<OutputInfo> parse_xrandr(const std::string& output) const;

    /// Parse wlr-randr output into OutputInfo structs.
    std::vector<OutputInfo> parse_wlr_randr(const std::string& output) const;

    /// Parse /sys/class/drm entries for EDID data.
    Result<OutputInfo, std::string> parse_edid(const std::string& drm_path) const;

    /// Get the user config directory, expanding ~ to $HOME.
    std::string config_dir() const;

    /// Read profile JSON from disk.
    Result<DisplayProfile, std::string> read_profile(const std::string& name) const;

    /// Write profile JSON to disk.
    Result<void, std::string> write_profile(const DisplayProfile& profile) const;

    /// Read night-mode config from disk.
    NightModeSettings read_night_config() const;

    /// Write night-mode config to disk.
    void write_night_config(const NightModeSettings& settings) const;
};

} // namespace straylight
