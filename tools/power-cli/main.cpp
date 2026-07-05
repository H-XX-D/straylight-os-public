// tools/power-cli/main.cpp
// straylight-power-cli — CLI for power management.
#include "../../services/power/power_engine.h"

#include <straylight/log.h>

#include <iomanip>
#include <iostream>
#include <string>

using straylight::PowerEngine;
using straylight::PowerProfile;
using straylight::BatteryStatus;
using straylight::BatteryHealth;

static void print_usage() {
    std::cerr
        << "straylight-power-cli — Power management\n\n"
        << "Usage:\n"
        << "  straylight-power-cli status               Full power status\n"
        << "  straylight-power-cli battery               Battery details\n"
        << "  straylight-power-cli suspend                Suspend the system\n"
        << "  straylight-power-cli hibernate              Hibernate the system\n"
        << "  straylight-power-cli brightness <level>     Set brightness (0-100)\n"
        << "  straylight-power-cli profile <name>         Set power profile\n"
        << "  straylight-power-cli wake <time>            Set wake timer\n"
        << "  straylight-power-cli lid <action>           Set lid close action\n"
        << "\nProfiles: performance, balanced, powersave\n"
        << "Lid actions: suspend, hibernate, lock, ignore\n"
        << "Wake time: +30m, +2h, 07:00\n"
        << "\nExamples:\n"
        << "  straylight-power-cli brightness 50\n"
        << "  straylight-power-cli profile powersave\n"
        << "  straylight-power-cli wake +30m\n"
        << "  straylight-power-cli lid hibernate\n";
}

static const char* status_str(BatteryStatus s) {
    switch (s) {
        case BatteryStatus::Charging:    return "Charging";
        case BatteryStatus::Discharging: return "Discharging";
        case BatteryStatus::Full:        return "Full";
        case BatteryStatus::NotCharging: return "Not Charging";
        default:                         return "Unknown";
    }
}

static const char* health_str(BatteryHealth h) {
    switch (h) {
        case BatteryHealth::Good:     return "Good";
        case BatteryHealth::Fair:     return "Fair";
        case BatteryHealth::Poor:     return "Poor";
        case BatteryHealth::Critical: return "Critical";
        default:                      return "Unknown";
    }
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

    PowerEngine engine;

    if (cmd == "status") {
        // Power source.
        auto source = engine.get_power_source();
        std::cout << "Power Source: ";
        switch (source) {
            case straylight::PowerSource::AC:      std::cout << "AC (plugged in)"; break;
            case straylight::PowerSource::Battery: std::cout << "Battery"; break;
            default:                               std::cout << "Unknown"; break;
        }
        std::cout << "\n";

        // Profile.
        auto prof = engine.get_profile();
        if (prof.has_value()) {
            std::cout << "Profile:      " << straylight::profile_str(prof.value()) << "\n";
        }

        // Brightness.
        auto br = engine.get_brightness();
        if (br.has_value()) {
            std::cout << "Brightness:   " << br.value() << "%\n";
        }

        // Battery summary.
        auto bat = engine.get_primary_battery();
        if (bat.has_value()) {
            const auto& b = bat.value();
            std::cout << "Battery:      " << b.capacity_percent << "% ("
                      << status_str(b.status) << ")";
            if (b.time_remaining_min >= 0) {
                int hours = b.time_remaining_min / 60;
                int mins = b.time_remaining_min % 60;
                std::cout << " — ";
                if (hours > 0) std::cout << hours << "h ";
                std::cout << mins << "m remaining";
            }
            std::cout << "\n";
            std::cout << "Health:       " << health_str(b.health);
            if (b.health_percent > 0) {
                std::cout << " (" << std::fixed << std::setprecision(1)
                          << b.health_percent << "%)";
            }
            std::cout << "\n";
        } else {
            std::cout << "Battery:      not present\n";
        }

        // Lid action.
        auto lid = engine.get_lid_action();
        if (lid.has_value()) {
            std::cout << "Lid Action:   ";
            switch (lid.value()) {
                case straylight::LidAction::Suspend:   std::cout << "suspend"; break;
                case straylight::LidAction::Hibernate: std::cout << "hibernate"; break;
                case straylight::LidAction::Lock:      std::cout << "lock"; break;
                case straylight::LidAction::Ignore:    std::cout << "ignore"; break;
            }
            std::cout << "\n";
        }

    } else if (cmd == "battery") {
        auto batteries = engine.get_batteries();
        if (!batteries.has_value()) {
            std::cerr << "Error: " << batteries.error().message() << "\n";
            return 1;
        }

        if (batteries.value().empty()) {
            std::cout << "No batteries detected.\n";
            return 0;
        }

        for (const auto& b : batteries.value()) {
            std::cout << "Battery: " << b.name << "\n";
            std::cout << "  Status:       " << status_str(b.status) << "\n";
            std::cout << "  Capacity:     " << b.capacity_percent << "%\n";

            if (b.energy_now_uwh > 0) {
                std::cout << "  Energy:       " << std::fixed << std::setprecision(2)
                          << b.energy_now_uwh / 1000000.0 << " Wh / "
                          << b.energy_full_uwh / 1000000.0 << " Wh\n";
            }
            if (b.energy_design_uwh > 0) {
                std::cout << "  Design Cap:   " << std::fixed << std::setprecision(2)
                          << b.energy_design_uwh / 1000000.0 << " Wh\n";
            }
            if (b.power_now_uw > 0) {
                std::cout << "  Power Draw:   " << std::fixed << std::setprecision(2)
                          << b.power_now_uw / 1000000.0 << " W\n";
            }
            if (b.voltage_now_uv > 0) {
                std::cout << "  Voltage:      " << std::fixed << std::setprecision(2)
                          << b.voltage_now_uv / 1000000.0 << " V\n";
            }
            if (b.temperature_c > 0) {
                std::cout << "  Temperature:  " << std::fixed << std::setprecision(1)
                          << b.temperature_c << " C\n";
            }
            if (!b.technology.empty()) {
                std::cout << "  Technology:   " << b.technology << "\n";
            }
            if (b.cycle_count >= 0) {
                std::cout << "  Cycles:       " << b.cycle_count << "\n";
            }
            std::cout << "  Health:       " << health_str(b.health);
            if (b.health_percent > 0) {
                std::cout << " (" << std::fixed << std::setprecision(1)
                          << b.health_percent << "%)";
            }
            std::cout << "\n";

            if (b.time_remaining_min >= 0) {
                int hours = b.time_remaining_min / 60;
                int mins = b.time_remaining_min % 60;
                std::cout << "  Time Left:    ";
                if (hours > 0) std::cout << hours << "h ";
                std::cout << mins << "m\n";
            }
            std::cout << "\n";
        }

    } else if (cmd == "suspend") {
        auto r = engine.suspend();
        if (!r.has_value()) {
            std::cerr << "Error: " << r.error().message() << "\n";
            return 1;
        }
        std::cout << "System suspended.\n";

    } else if (cmd == "hibernate") {
        auto r = engine.hibernate();
        if (!r.has_value()) {
            std::cerr << "Error: " << r.error().message() << "\n";
            return 1;
        }
        std::cout << "System hibernated.\n";

    } else if (cmd == "brightness") {
        if (argc < 3) {
            // Show current brightness.
            auto r = engine.get_brightness();
            if (!r.has_value()) {
                std::cerr << "Error: " << r.error().message() << "\n";
                return 1;
            }
            std::cout << "Brightness: " << r.value() << "%\n";
            return 0;
        }

        int level = 0;
        try { level = std::stoi(argv[2]); } catch (...) {
            std::cerr << "Error: invalid brightness level\n";
            return 1;
        }

        auto r = engine.set_brightness(level);
        if (!r.has_value()) {
            std::cerr << "Error: " << r.error().message() << "\n";
            return 1;
        }
        std::cout << "Brightness set to " << level << "%\n";

    } else if (cmd == "profile") {
        if (argc < 3) {
            auto r = engine.get_profile();
            if (!r.has_value()) {
                std::cerr << "Error: " << r.error().message() << "\n";
                return 1;
            }
            std::cout << "Profile: " << straylight::profile_str(r.value()) << "\n";
            return 0;
        }

        std::string name = argv[2];
        if (name != "performance" && name != "balanced" && name != "powersave") {
            std::cerr << "Error: unknown profile '" << name
                      << "' (valid: performance, balanced, powersave)\n";
            return 1;
        }

        auto r = engine.set_profile(straylight::parse_profile(name));
        if (!r.has_value()) {
            std::cerr << "Error: " << r.error().message() << "\n";
            return 1;
        }
        std::cout << "Profile set to " << name << "\n";

    } else if (cmd == "wake") {
        if (argc < 3) {
            std::cerr << "Usage: straylight-power-cli wake <time>\n";
            std::cerr << "Examples: +30m, +2h, 07:00\n";
            return 1;
        }

        auto r = engine.set_wake_timer(argv[2]);
        if (!r.has_value()) {
            std::cerr << "Error: " << r.error().message() << "\n";
            return 1;
        }
        std::cout << "Wake timer set for " << argv[2] << "\n";

    } else if (cmd == "lid") {
        if (argc < 3) {
            auto r = engine.get_lid_action();
            if (!r.has_value()) {
                std::cerr << "Error: " << r.error().message() << "\n";
                return 1;
            }
            std::cout << "Lid action: ";
            switch (r.value()) {
                case straylight::LidAction::Suspend:   std::cout << "suspend"; break;
                case straylight::LidAction::Hibernate: std::cout << "hibernate"; break;
                case straylight::LidAction::Lock:      std::cout << "lock"; break;
                case straylight::LidAction::Ignore:    std::cout << "ignore"; break;
            }
            std::cout << "\n";
            return 0;
        }

        std::string action = argv[2];
        straylight::LidAction lid_action;
        if (action == "suspend")        lid_action = straylight::LidAction::Suspend;
        else if (action == "hibernate") lid_action = straylight::LidAction::Hibernate;
        else if (action == "lock")      lid_action = straylight::LidAction::Lock;
        else if (action == "ignore")    lid_action = straylight::LidAction::Ignore;
        else {
            std::cerr << "Error: unknown lid action '" << action
                      << "' (valid: suspend, hibernate, lock, ignore)\n";
            return 1;
        }

        auto r = engine.set_lid_action(lid_action);
        if (!r.has_value()) {
            std::cerr << "Error: " << r.error().message() << "\n";
            return 1;
        }
        std::cout << "Lid action set to " << action << "\n";

    } else {
        std::cerr << "Error: unknown command '" << cmd << "'\n";
        print_usage();
        return 1;
    }

    return 0;
}
