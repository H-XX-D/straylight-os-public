/**
 * thermal-cli — Command-line interface for straylight-thermal daemon.
 *
 * Commands:
 *   status              Show current thermal state summary
 *   zones               List all thermal zones with temperatures
 *   history             Show recent thermal events
 *   predict             Show predicted temperatures for all zones
 *   set-policy <p>      Set throttle policy: aggressive, balanced, relaxed
 */

#include "../../services/thermal/thermal_model.h"
#include "../../services/thermal/throttle_controller.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace straylight::thermal;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void print_usage() {
    fprintf(stderr,
        "Usage: thermal-cli <command> [args]\n"
        "\n"
        "Commands:\n"
        "  status              Show current thermal state summary\n"
        "  zones               List all thermal zones with temperatures\n"
        "  history             Show recent thermal events from log\n"
        "  predict             Show predicted temperatures for all zones\n"
        "  set-policy <p>      Set throttle policy (aggressive|balanced|relaxed)\n"
        "\n");
}

static std::string read_file_line(const std::string& path) {
    std::ifstream f(path);
    std::string line;
    if (f) std::getline(f, line);
    return line;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

static int cmd_status() {
    ThermalModel model;
    auto discover = model.discover_zones();
    if (!discover) {
        fprintf(stderr, "error: %s\n", discover.err().c_str());
        return 1;
    }

    auto poll = model.poll();
    if (!poll) {
        fprintf(stderr, "error: %s\n", poll.err().c_str());
        return 1;
    }

    // Load config for state calculation.
    ThermalConfig config;
    auto cfg_result = ThermalConfig::load("/etc/straylight/thermal.conf");
    if (cfg_result) config = cfg_result.value();

    ThermalState state = model.get_overall_thermal_state(config);

    printf("Thermal Status\n");
    printf("==============\n");
    printf("  Overall State : %s\n", state_to_string(state));
    printf("  Zones         : %zu\n", model.zones().size());
    printf("  Thresholds    : warn=%dC throttle=%dC critical=%dC\n",
           config.warn_temp, config.throttle_temp, config.critical_temp);
    printf("  Prediction    : %s (horizon=%ds)\n",
           config.enable_prediction ? "enabled" : "disabled",
           config.prediction_horizon_s);

    // Check throttle state from runtime file.
    std::string throttle_state = "inactive";
    if (std::filesystem::exists("/run/straylight/compositor/target_fps")) {
        std::string fps = read_file_line("/run/straylight/compositor/target_fps");
        if (!fps.empty() && std::stoi(fps) < 60) {
            throttle_state = "active";
        }
    }
    printf("  Throttle      : %s\n", throttle_state.c_str());

    printf("\n");

    // Show hottest zone.
    int max_temp = 0;
    std::string hottest;
    for (const auto& z : model.zones()) {
        if (z.current_temp > max_temp) {
            max_temp = z.current_temp;
            hottest = z.name;
        }
    }
    printf("  Hottest Zone  : %s (%dC)\n", hottest.c_str(), max_temp);

    return 0;
}

static int cmd_zones() {
    ThermalModel model;
    auto discover = model.discover_zones();
    if (!discover) {
        fprintf(stderr, "error: %s\n", discover.err().c_str());
        return 1;
    }

    auto poll = model.poll();
    if (!poll) {
        fprintf(stderr, "error: %s\n", poll.err().c_str());
        return 1;
    }

    ThermalConfig config;
    auto cfg_result = ThermalConfig::load("/etc/straylight/thermal.conf");
    if (cfg_result) config = cfg_result.value();

    printf("%-25s %-10s %6s  %-8s  %-10s  Trip Points\n",
           "Zone", "Type", "Temp", "Trend", "Predicted");
    printf("%-25s %-10s %6s  %-8s  %-10s  -----------\n",
           "----", "----", "----", "-----", "---------");

    for (const auto& zone : model.zones()) {
        double predicted = model.predict_temperature(zone, config.prediction_horizon_s);

        std::string trips;
        for (const auto& tp : zone.trip_points) {
            if (!trips.empty()) trips += ", ";
            trips += tp.type + "=" + std::to_string(tp.temperature) + "C";
        }
        if (trips.empty()) trips = "none";

        printf("%-25s %-10s %4dC   %-8s  %6.1fC     %s\n",
               zone.name.c_str(),
               zone.type.c_str(),
               zone.current_temp,
               trend_to_string(zone.trend),
               predicted,
               trips.c_str());
    }

    return 0;
}

static int cmd_history() {
    const std::string log_path = "/var/log/straylight/thermal.log";
    const std::string fallback = "/tmp/straylight-thermal.log";

    std::string path = log_path;
    if (!std::filesystem::exists(path)) {
        path = fallback;
    }

    std::ifstream logfile(path);
    if (!logfile.is_open()) {
        fprintf(stderr, "No thermal log found at %s or %s\n",
                log_path.c_str(), fallback.c_str());
        fprintf(stderr, "The straylight-thermal daemon may not be running.\n");
        return 1;
    }

    // Show last 50 lines.
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(logfile, line)) {
        lines.push_back(line);
    }

    printf("Thermal Event History (last 50 entries)\n");
    printf("=======================================\n");

    size_t start = 0;
    if (lines.size() > 50) {
        start = lines.size() - 50;
    }

    for (size_t i = start; i < lines.size(); ++i) {
        printf("%s\n", lines[i].c_str());
    }

    if (lines.empty()) {
        printf("  (no events recorded)\n");
    }

    return 0;
}

static int cmd_predict() {
    ThermalModel model;
    auto discover = model.discover_zones();
    if (!discover) {
        fprintf(stderr, "error: %s\n", discover.err().c_str());
        return 1;
    }

    // Poll multiple times to build history for prediction.
    printf("Collecting samples for prediction (5 seconds)...\n");
    for (int i = 0; i < 5; ++i) {
        auto poll = model.poll();
        if (!poll) {
            fprintf(stderr, "poll error: %s\n", poll.err().c_str());
        }
        if (i < 4) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    ThermalConfig config;
    auto cfg_result = ThermalConfig::load("/etc/straylight/thermal.conf");
    if (cfg_result) config = cfg_result.value();

    printf("\nTemperature Predictions (horizon: %ds)\n", config.prediction_horizon_s);
    printf("======================================\n");
    printf("%-25s  %6s  %10s  %10s  %-8s\n",
           "Zone", "Now", "+5s", "+10s", "Trend");
    printf("%-25s  %6s  %10s  %10s  %-8s\n",
           "----", "---", "---", "----", "-----");

    for (const auto& zone : model.zones()) {
        double pred_5 = model.predict_temperature(zone, 5);
        double pred_10 = model.predict_temperature(zone, 10);

        printf("%-25s  %4dC   %6.1fC     %6.1fC     %-8s\n",
               zone.name.c_str(),
               zone.current_temp,
               pred_5,
               pred_10,
               trend_to_string(zone.trend));
    }

    return 0;
}

static int cmd_set_policy(const std::string& policy_str) {
    ThrottlePolicy policy = policy_from_string(policy_str);

    // Write policy to a runtime file that the daemon reads on SIGHUP.
    const std::string policy_path = "/run/straylight/thermal_policy";
    namespace fs = std::filesystem;

    std::error_code ec;
    fs::create_directories("/run/straylight", ec);

    std::ofstream pf(policy_path);
    if (!pf) {
        // Try fallback.
        std::string fallback = "/tmp/straylight-thermal-policy";
        pf.open(fallback);
        if (!pf) {
            fprintf(stderr, "error: cannot write policy file\n");
            return 1;
        }
    }
    pf << policy_to_string(policy);
    pf.close();

    printf("Throttle policy set to: %s\n", policy_to_string(policy));

    // Signal the daemon to reload.
    std::string pid_path = "/var/run/straylight/straylight-thermal.pid";
    std::ifstream pidf(pid_path);
    if (pidf) {
        int pid = 0;
        pidf >> pid;
        if (pid > 0) {
            if (kill(pid, SIGHUP) == 0) {
                printf("Sent SIGHUP to straylight-thermal (pid %d)\n", pid);
            } else {
                fprintf(stderr, "warning: could not signal daemon (pid %d)\n", pid);
            }
        }
    } else {
        fprintf(stderr, "warning: daemon may not be running (no PID file)\n");
    }

    return 0;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string cmd = argv[1];

    if (cmd == "status") {
        return cmd_status();
    } else if (cmd == "zones") {
        return cmd_zones();
    } else if (cmd == "history") {
        return cmd_history();
    } else if (cmd == "predict") {
        return cmd_predict();
    } else if (cmd == "set-policy") {
        if (argc < 3) {
            fprintf(stderr, "Usage: thermal-cli set-policy <aggressive|balanced|relaxed>\n");
            return 1;
        }
        return cmd_set_policy(argv[2]);
    } else if (cmd == "--help" || cmd == "-h") {
        print_usage();
        return 0;
    } else {
        fprintf(stderr, "Unknown command: %s\n", cmd.c_str());
        print_usage();
        return 1;
    }
}
