// services/power/power_engine.cpp
#include "power_engine.h"

#include <straylight/log.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace straylight {

// ---------------------------------------------------------------------------
// Profile helpers
// ---------------------------------------------------------------------------

PowerProfile parse_profile(const std::string& str) {
    if (str == "performance") return PowerProfile::Performance;
    if (str == "powersave")   return PowerProfile::Powersave;
    return PowerProfile::Balanced;
}

const char* profile_str(PowerProfile p) {
    switch (p) {
        case PowerProfile::Performance: return "performance";
        case PowerProfile::Balanced:    return "balanced";
        case PowerProfile::Powersave:   return "powersave";
    }
    return "balanced";
}

// ---------------------------------------------------------------------------
// Sysfs helpers
// ---------------------------------------------------------------------------

std::string PowerEngine::read_sysfs(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs) return "";
    std::string val;
    std::getline(ifs, val);
    // Trim trailing whitespace.
    while (!val.empty() && std::isspace(static_cast<unsigned char>(val.back()))) {
        val.pop_back();
    }
    return val;
}

int64_t PowerEngine::read_sysfs_int(const std::string& path) {
    std::string val = read_sysfs(path);
    if (val.empty()) return -1;
    try { return std::stoll(val); } catch (...) { return -1; }
}

std::string PowerEngine::find_backlight() {
    std::string base = "/sys/class/backlight";
    if (!fs::exists(base)) return "";

    std::error_code ec;
    for (auto& entry : fs::directory_iterator(base, ec)) {
        return entry.path().string();
    }
    return "";
}

std::string PowerEngine::find_ac_supply() {
    std::string base = "/sys/class/power_supply";
    if (!fs::exists(base)) return "";

    std::error_code ec;
    for (auto& entry : fs::directory_iterator(base, ec)) {
        std::string type = read_sysfs(entry.path().string() + "/type");
        if (type == "Mains") return entry.path().string();
    }
    return "";
}

// ---------------------------------------------------------------------------
// Battery
// ---------------------------------------------------------------------------

Result<std::vector<BatteryInfo>, SLError> PowerEngine::get_batteries() const {
    std::string base = "/sys/class/power_supply";
    if (!fs::exists(base)) {
        return Result<std::vector<BatteryInfo>, SLError>::error(
            {SLErrorCode::NotFound, "/sys/class/power_supply not found"});
    }

    std::vector<BatteryInfo> batteries;
    std::error_code ec;

    for (auto& entry : fs::directory_iterator(base, ec)) {
        std::string type = read_sysfs(entry.path().string() + "/type");
        if (type != "Battery") continue;

        BatteryInfo bat;
        bat.name = entry.path().filename().string();

        // Status.
        std::string status = read_sysfs(entry.path().string() + "/status");
        if (status == "Charging") bat.status = BatteryStatus::Charging;
        else if (status == "Discharging") bat.status = BatteryStatus::Discharging;
        else if (status == "Full") bat.status = BatteryStatus::Full;
        else if (status == "Not charging") bat.status = BatteryStatus::NotCharging;
        else bat.status = BatteryStatus::Unknown;

        // Capacity.
        bat.capacity_percent = static_cast<int>(
            read_sysfs_int(entry.path().string() + "/capacity"));

        // Energy values (try energy_now first, then charge_now * voltage_now).
        int64_t energy_now = read_sysfs_int(entry.path().string() + "/energy_now");
        int64_t energy_full = read_sysfs_int(entry.path().string() + "/energy_full");
        int64_t energy_design = read_sysfs_int(entry.path().string() + "/energy_full_design");

        if (energy_now < 0) {
            // Try charge-based values.
            int64_t charge_now = read_sysfs_int(entry.path().string() + "/charge_now");
            int64_t charge_full = read_sysfs_int(entry.path().string() + "/charge_full");
            int64_t charge_design = read_sysfs_int(entry.path().string() + "/charge_full_design");
            int64_t voltage = read_sysfs_int(entry.path().string() + "/voltage_now");

            if (charge_now >= 0 && voltage > 0) {
                energy_now = charge_now * voltage / 1000000;
                energy_full = charge_full * voltage / 1000000;
                energy_design = charge_design * voltage / 1000000;
            }
        }

        bat.energy_now_uwh = (energy_now >= 0) ? static_cast<uint64_t>(energy_now) : 0;
        bat.energy_full_uwh = (energy_full >= 0) ? static_cast<uint64_t>(energy_full) : 0;
        bat.energy_design_uwh = (energy_design >= 0) ? static_cast<uint64_t>(energy_design) : 0;

        // Power draw.
        bat.power_now_uw = static_cast<int32_t>(
            read_sysfs_int(entry.path().string() + "/power_now"));

        // Voltage.
        bat.voltage_now_uv = static_cast<int32_t>(
            read_sysfs_int(entry.path().string() + "/voltage_now"));

        // Temperature (in tenths of degree Celsius).
        int64_t temp = read_sysfs_int(entry.path().string() + "/temp");
        if (temp >= 0) bat.temperature_c = temp / 10.0;

        // Technology.
        bat.technology = read_sysfs(entry.path().string() + "/technology");

        // Cycle count.
        int64_t cycles = read_sysfs_int(entry.path().string() + "/cycle_count");
        bat.cycle_count = (cycles >= 0) ? static_cast<int>(cycles) : -1;

        // Health assessment.
        if (bat.energy_design_uwh > 0 && bat.energy_full_uwh > 0) {
            bat.health_percent =
                static_cast<double>(bat.energy_full_uwh) / bat.energy_design_uwh * 100.0;

            if (bat.health_percent > 80.0) bat.health = BatteryHealth::Good;
            else if (bat.health_percent > 60.0) bat.health = BatteryHealth::Fair;
            else if (bat.health_percent > 40.0) bat.health = BatteryHealth::Poor;
            else bat.health = BatteryHealth::Critical;
        }

        // Time remaining estimation.
        if (bat.power_now_uw > 0 && bat.status == BatteryStatus::Discharging) {
            bat.time_remaining_min =
                static_cast<int>(bat.energy_now_uwh * 60.0 / bat.power_now_uw);
        } else if (bat.power_now_uw > 0 && bat.status == BatteryStatus::Charging) {
            uint64_t remaining_energy = bat.energy_full_uwh - bat.energy_now_uwh;
            bat.time_remaining_min =
                static_cast<int>(remaining_energy * 60.0 / bat.power_now_uw);
        }

        batteries.push_back(bat);
    }

    return Result<std::vector<BatteryInfo>, SLError>::ok(std::move(batteries));
}

Result<BatteryInfo, SLError> PowerEngine::get_primary_battery() const {
    auto batteries = get_batteries();
    if (!batteries.has_value()) return Result<BatteryInfo, SLError>::error(batteries.error());
    if (batteries.value().empty()) {
        return Result<BatteryInfo, SLError>::error(
            {SLErrorCode::NotFound, "No battery found"});
    }
    return Result<BatteryInfo, SLError>::ok(batteries.value()[0]);
}

PowerSource PowerEngine::get_power_source() const {
    auto ac_path = find_ac_supply();
    if (ac_path.empty()) return PowerSource::Unknown;

    std::string online = read_sysfs(ac_path + "/online");
    if (online == "1") return PowerSource::AC;
    return PowerSource::Battery;
}

// ---------------------------------------------------------------------------
// Suspend / Hibernate
// ---------------------------------------------------------------------------

Result<void, SLError> PowerEngine::suspend() const {
    SL_INFO("power: suspending system");
    int rc = std::system("systemctl suspend 2>&1");
    if (rc != 0) {
        // Fallback: write to /sys/power/state.
        std::ofstream ofs("/sys/power/state");
        if (ofs) {
            ofs << "mem";
            return Result<void, SLError>::ok();
        }
        return Result<void, SLError>::error(
            {SLErrorCode::IOError, "Suspend failed"});
    }
    return Result<void, SLError>::ok();
}

Result<void, SLError> PowerEngine::hibernate() const {
    SL_INFO("power: hibernating system");
    int rc = std::system("systemctl hibernate 2>&1");
    if (rc != 0) {
        std::ofstream ofs("/sys/power/state");
        if (ofs) {
            ofs << "disk";
            return Result<void, SLError>::ok();
        }
        return Result<void, SLError>::error(
            {SLErrorCode::IOError, "Hibernate failed"});
    }
    return Result<void, SLError>::ok();
}

// ---------------------------------------------------------------------------
// Wake timer
// ---------------------------------------------------------------------------

Result<void, SLError> PowerEngine::set_wake_timer(const std::string& time_spec) const {
    // time_spec can be "+30m", "+2h", "HH:MM", or epoch timestamp.
    std::string mode = "-s";  // seconds from now
    std::string value = time_spec;

    if (time_spec[0] == '+') {
        // Parse relative time: +30m, +2h, +1h30m.
        std::string spec = time_spec.substr(1);
        int total_seconds = 0;

        size_t pos = 0;
        while (pos < spec.size()) {
            size_t num_end = pos;
            while (num_end < spec.size() && std::isdigit(spec[num_end])) ++num_end;
            if (num_end == pos) break;

            int num = std::stoi(spec.substr(pos, num_end - pos));
            char unit = (num_end < spec.size()) ? spec[num_end] : 's';

            switch (unit) {
                case 'h': total_seconds += num * 3600; break;
                case 'm': total_seconds += num * 60; break;
                case 's': total_seconds += num; break;
                default: total_seconds += num; break;
            }

            pos = num_end + 1;
        }

        value = std::to_string(total_seconds);
    } else if (time_spec.find(':') != std::string::npos) {
        mode = "-t";  // absolute time
        // Parse HH:MM and convert to epoch.
        int h = 0, m = 0;
        std::sscanf(time_spec.c_str(), "%d:%d", &h, &m);
        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);
        std::tm tm;
        localtime_r(&time_t_now, &tm);
        tm.tm_hour = h;
        tm.tm_min = m;
        tm.tm_sec = 0;
        auto wake_time = std::mktime(&tm);
        // If the time is in the past, add 24 hours.
        if (wake_time <= time_t_now) wake_time += 86400;
        value = std::to_string(wake_time);
    }

    std::string cmd = "rtcwake -m no " + mode + " " + value + " 2>&1";
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        return Result<void, SLError>::error(
            {SLErrorCode::IOError, "rtcwake failed — is it installed?"});
    }

    SL_INFO("power: wake timer set for {}", time_spec);
    return Result<void, SLError>::ok();
}

// ---------------------------------------------------------------------------
// Power profiles
// ---------------------------------------------------------------------------

Result<void, SLError> PowerEngine::set_profile(PowerProfile profile) const {
    const char* name = profile_str(profile);

    // Try power-profiles-daemon first.
    std::string cmd = "powerprofilesctl set " + std::string(name) + " 2>/dev/null";
    int rc = std::system(cmd.c_str());
    if (rc == 0) {
        SL_INFO("power: set profile to {} via powerprofilesctl", name);
        return Result<void, SLError>::ok();
    }

    // Fallback: set CPU governor directly.
    std::string governor;
    switch (profile) {
        case PowerProfile::Performance: governor = "performance"; break;
        case PowerProfile::Powersave:   governor = "powersave"; break;
        default:                        governor = "schedutil"; break;
    }

    // Apply to all CPU cores.
    std::string gov_base = "/sys/devices/system/cpu";
    if (fs::exists(gov_base)) {
        std::error_code ec;
        for (auto& entry : fs::directory_iterator(gov_base, ec)) {
            std::string name_str = entry.path().filename().string();
            if (name_str.substr(0, 3) != "cpu" || !std::isdigit(name_str[3])) continue;

            std::string gov_path = entry.path().string() + "/cpufreq/scaling_governor";
            std::ofstream ofs(gov_path);
            if (ofs) {
                ofs << governor;
            }
        }
    }

    // Also set the energy_performance_preference if available.
    std::string epp;
    switch (profile) {
        case PowerProfile::Performance: epp = "performance"; break;
        case PowerProfile::Powersave:   epp = "power"; break;
        default:                        epp = "balance_performance"; break;
    }

    if (fs::exists(gov_base)) {
        std::error_code ec;
        for (auto& entry : fs::directory_iterator(gov_base, ec)) {
            std::string name_str = entry.path().filename().string();
            if (name_str.substr(0, 3) != "cpu" || !std::isdigit(name_str[3])) continue;

            std::string epp_path = entry.path().string() +
                                   "/cpufreq/energy_performance_preference";
            if (fs::exists(epp_path)) {
                std::ofstream ofs(epp_path);
                if (ofs) ofs << epp;
            }
        }
    }

    SL_INFO("power: set profile to {} (governor={})", name, governor);
    return Result<void, SLError>::ok();
}

Result<PowerProfile, SLError> PowerEngine::get_profile() const {
    // Try power-profiles-daemon.
    FILE* fp = popen("powerprofilesctl get 2>/dev/null", "r");
    if (fp) {
        char buf[64] = {};
        if (fgets(buf, sizeof(buf), fp)) {
            pclose(fp);
            std::string prof = buf;
            while (!prof.empty() && std::isspace(prof.back())) prof.pop_back();
            return Result<PowerProfile, SLError>::ok(parse_profile(prof));
        }
        pclose(fp);
    }

    // Fallback: read governor from CPU0.
    std::string gov = read_sysfs("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
    if (gov == "performance") return Result<PowerProfile, SLError>::ok(PowerProfile::Performance);
    if (gov == "powersave")   return Result<PowerProfile, SLError>::ok(PowerProfile::Powersave);
    return Result<PowerProfile, SLError>::ok(PowerProfile::Balanced);
}

// ---------------------------------------------------------------------------
// Brightness
// ---------------------------------------------------------------------------

Result<void, SLError> PowerEngine::set_brightness(int level) const {
    auto bl = find_backlight();
    if (bl.empty()) {
        return Result<void, SLError>::error(
            {SLErrorCode::NotFound, "No backlight device found"});
    }

    int64_t max_br = read_sysfs_int(bl + "/max_brightness");
    if (max_br <= 0) max_br = 100;

    int target = std::clamp(level, 0, 100);
    int hw_value = static_cast<int>(target * max_br / 100);

    std::ofstream ofs(bl + "/brightness");
    if (!ofs) {
        // Try via brightnessctl.
        std::string cmd = "brightnessctl set " + std::to_string(target) + "% 2>/dev/null";
        int rc = std::system(cmd.c_str());
        if (rc != 0) {
            return Result<void, SLError>::error(
                {SLErrorCode::PermissionDenied, "Cannot set brightness"});
        }
        return Result<void, SLError>::ok();
    }
    ofs << hw_value;

    SL_INFO("power: brightness set to {}%", target);
    return Result<void, SLError>::ok();
}

Result<int, SLError> PowerEngine::get_brightness() const {
    auto bl = find_backlight();
    if (bl.empty()) {
        return Result<int, SLError>::error(
            {SLErrorCode::NotFound, "No backlight device found"});
    }

    int64_t current = read_sysfs_int(bl + "/brightness");
    int64_t max_br = read_sysfs_int(bl + "/max_brightness");

    if (current < 0 || max_br <= 0) {
        return Result<int, SLError>::error(
            {SLErrorCode::IOError, "Cannot read brightness"});
    }

    int percent = static_cast<int>(current * 100 / max_br);
    return Result<int, SLError>::ok(percent);
}

Result<int, SLError> PowerEngine::get_max_brightness() const {
    auto bl = find_backlight();
    if (bl.empty()) {
        return Result<int, SLError>::error(
            {SLErrorCode::NotFound, "No backlight device found"});
    }
    int64_t max_br = read_sysfs_int(bl + "/max_brightness");
    return Result<int, SLError>::ok(static_cast<int>(max_br));
}

// ---------------------------------------------------------------------------
// Lid
// ---------------------------------------------------------------------------

Result<void, SLError> PowerEngine::set_lid_action(LidAction action) const {
    std::string action_str;
    switch (action) {
        case LidAction::Suspend:   action_str = "suspend"; break;
        case LidAction::Hibernate: action_str = "hibernate"; break;
        case LidAction::Lock:      action_str = "lock"; break;
        case LidAction::Ignore:    action_str = "ignore"; break;
    }

    // Write to logind.conf.
    std::string conf_path = "/etc/systemd/logind.conf";
    std::ifstream ifs(conf_path);
    if (!ifs) {
        return Result<void, SLError>::error(
            {SLErrorCode::IOError, "Cannot read " + conf_path});
    }

    std::string content((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());
    ifs.close();

    // Replace or add HandleLidSwitch.
    std::string key = "HandleLidSwitch=";
    auto pos = content.find(key);
    if (pos != std::string::npos) {
        auto end = content.find('\n', pos);
        content.replace(pos, end - pos, key + action_str);
    } else {
        // Find [Login] section.
        auto login_pos = content.find("[Login]");
        if (login_pos != std::string::npos) {
            auto newline = content.find('\n', login_pos);
            content.insert(newline + 1, key + action_str + "\n");
        } else {
            content += "\n[Login]\n" + key + action_str + "\n";
        }
    }

    std::ofstream ofs(conf_path, std::ios::trunc);
    if (!ofs) {
        return Result<void, SLError>::error(
            {SLErrorCode::PermissionDenied, "Cannot write " + conf_path});
    }
    ofs << content;

    // Reload logind.
    std::system("systemctl restart systemd-logind 2>/dev/null");

    SL_INFO("power: lid action set to {}", action_str);
    return Result<void, SLError>::ok();
}

Result<LidAction, SLError> PowerEngine::get_lid_action() const {
    std::ifstream ifs("/etc/systemd/logind.conf");
    if (!ifs) {
        return Result<LidAction, SLError>::ok(LidAction::Suspend); // default
    }

    std::string line;
    while (std::getline(ifs, line)) {
        if (line.find("HandleLidSwitch=") == 0) {
            std::string val = line.substr(16);
            if (val == "hibernate") return Result<LidAction, SLError>::ok(LidAction::Hibernate);
            if (val == "lock")      return Result<LidAction, SLError>::ok(LidAction::Lock);
            if (val == "ignore")    return Result<LidAction, SLError>::ok(LidAction::Ignore);
            return Result<LidAction, SLError>::ok(LidAction::Suspend);
        }
    }
    return Result<LidAction, SLError>::ok(LidAction::Suspend);
}

// ---------------------------------------------------------------------------
// USB power management
// ---------------------------------------------------------------------------

Result<std::vector<UsbPowerInfo>, SLError> PowerEngine::list_usb_power() const {
    std::string base = "/sys/bus/usb/devices";
    if (!fs::exists(base)) {
        return Result<std::vector<UsbPowerInfo>, SLError>::error(
            {SLErrorCode::NotFound, "/sys/bus/usb/devices not found"});
    }

    std::vector<UsbPowerInfo> devices;
    std::error_code ec;

    for (auto& entry : fs::directory_iterator(base, ec)) {
        auto power_dir = entry.path() / "power";
        if (!fs::exists(power_dir)) continue;

        UsbPowerInfo info;
        info.path = entry.path().string();
        info.product = read_sysfs(entry.path().string() + "/product");
        info.control = read_sysfs(power_dir.string() + "/control");
        info.autosuspend = (info.control == "auto");

        if (!info.product.empty()) {
            devices.push_back(info);
        }
    }

    return Result<std::vector<UsbPowerInfo>, SLError>::ok(std::move(devices));
}

Result<void, SLError> PowerEngine::set_usb_autosuspend(const std::string& device_path,
                                                        bool enable) const {
    std::string control_path = device_path + "/power/control";
    std::ofstream ofs(control_path);
    if (!ofs) {
        return Result<void, SLError>::error(
            {SLErrorCode::PermissionDenied, "Cannot write " + control_path});
    }
    ofs << (enable ? "auto" : "on");
    return Result<void, SLError>::ok();
}

// ---------------------------------------------------------------------------
// Low battery check
// ---------------------------------------------------------------------------

bool PowerEngine::check_low_battery(const BatteryInfo& battery) const {
    if (battery.status != BatteryStatus::Discharging) return false;

    if (battery.capacity_percent <= 5) {
        SL_CRITICAL("power: battery critically low ({}%), hibernating!", battery.capacity_percent);
        hibernate();
        return true;
    }

    if (battery.capacity_percent <= 20) {
        SL_WARN("power: battery low ({}%)", battery.capacity_percent);
        // Send a notification via the IPC system.
        std::string cmd = "straylight-notify-cli send 'Low Battery' "
                          "--body='Battery at " + std::to_string(battery.capacity_percent) +
                          "%. Connect charger.' --urgency=critical 2>/dev/null &";
        std::system(cmd.c_str());
        return true;
    }

    return false;
}

} // namespace straylight
