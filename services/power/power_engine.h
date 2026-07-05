// services/power/power_engine.h
#pragma once

#include <straylight/result.h>
#include <straylight/error.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace straylight {

/// Power source type.
enum class PowerSource : uint8_t {
    AC      = 0,
    Battery = 1,
    USB     = 2,
    Unknown = 3,
};

/// Battery status.
enum class BatteryStatus : uint8_t {
    Charging    = 0,
    Discharging = 1,
    Full        = 2,
    NotCharging = 3,
    Unknown     = 4,
};

/// Battery health assessment.
enum class BatteryHealth : uint8_t {
    Good      = 0,
    Fair      = 1,
    Poor      = 2,
    Critical  = 3,
    Unknown   = 4,
};

/// Battery information.
struct BatteryInfo {
    std::string name;               // e.g., "BAT0"
    BatteryStatus status = BatteryStatus::Unknown;
    int capacity_percent = 0;       // 0-100
    uint64_t energy_now_uwh = 0;    // Current energy in microwatt-hours
    uint64_t energy_full_uwh = 0;   // Full charge capacity
    uint64_t energy_design_uwh = 0; // Design capacity
    int32_t power_now_uw = 0;       // Current power draw in microwatts
    int32_t voltage_now_uv = 0;     // Current voltage in microvolts
    double temperature_c = 0.0;
    BatteryHealth health = BatteryHealth::Unknown;
    double health_percent = 0.0;    // energy_full / energy_design * 100
    int time_remaining_min = -1;    // Estimated minutes remaining (-1 = unknown)
    std::string technology;         // "Li-ion", "Li-poly", etc.
    int cycle_count = -1;
};

/// Power profile.
enum class PowerProfile : uint8_t {
    Performance = 0,
    Balanced    = 1,
    Powersave   = 2,
};

/// Parse a PowerProfile from string.
PowerProfile parse_profile(const std::string& str);
const char* profile_str(PowerProfile p);

/// Lid action.
enum class LidAction : uint8_t {
    Suspend   = 0,
    Hibernate = 1,
    Lock      = 2,
    Ignore    = 3,
};

/// USB device power info.
struct UsbPowerInfo {
    std::string path;
    std::string product;
    std::string control;    // "auto" or "on"
    bool autosuspend = false;
};

/// Power engine: battery monitoring, suspend/hibernate, profiles, brightness.
class PowerEngine {
public:
    // -----------------------------------------------------------------------
    // Battery
    // -----------------------------------------------------------------------

    /// Get info for all batteries.
    Result<std::vector<BatteryInfo>, SLError> get_batteries() const;

    /// Get primary battery info.
    Result<BatteryInfo, SLError> get_primary_battery() const;

    /// Get the current power source.
    PowerSource get_power_source() const;

    // -----------------------------------------------------------------------
    // Suspend / Hibernate
    // -----------------------------------------------------------------------

    Result<void, SLError> suspend() const;
    Result<void, SLError> hibernate() const;

    // -----------------------------------------------------------------------
    // Wake timer
    // -----------------------------------------------------------------------

    /// Set a wake timer using rtcwake.
    Result<void, SLError> set_wake_timer(const std::string& time_spec) const;

    // -----------------------------------------------------------------------
    // Power profiles
    // -----------------------------------------------------------------------

    Result<void, SLError> set_profile(PowerProfile profile) const;
    Result<PowerProfile, SLError> get_profile() const;

    // -----------------------------------------------------------------------
    // Brightness
    // -----------------------------------------------------------------------

    Result<void, SLError> set_brightness(int level) const;
    Result<int, SLError> get_brightness() const;
    Result<int, SLError> get_max_brightness() const;

    // -----------------------------------------------------------------------
    // Lid
    // -----------------------------------------------------------------------

    Result<void, SLError> set_lid_action(LidAction action) const;
    Result<LidAction, SLError> get_lid_action() const;

    // -----------------------------------------------------------------------
    // USB power management
    // -----------------------------------------------------------------------

    Result<std::vector<UsbPowerInfo>, SLError> list_usb_power() const;
    Result<void, SLError> set_usb_autosuspend(const std::string& device_path, bool enable) const;

    // -----------------------------------------------------------------------
    // Low battery actions
    // -----------------------------------------------------------------------

    /// Check battery and take action if below thresholds.
    /// Returns true if an action was taken.
    bool check_low_battery(const BatteryInfo& battery) const;

private:
    /// Read a sysfs file as string, trimmed.
    static std::string read_sysfs(const std::string& path);

    /// Read a sysfs file as integer.
    static int64_t read_sysfs_int(const std::string& path);

    /// Find the first backlight device.
    static std::string find_backlight();

    /// Find a power_supply directory for AC.
    static std::string find_ac_supply();
};

} // namespace straylight
