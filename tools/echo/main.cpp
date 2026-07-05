// tools/echo/main.cpp
// straylight-echo — system-wide undo via unified snapshot + rewind + replay.

#include "echo_engine.h"

#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

static void print_usage() {
    std::cerr
        << "straylight-echo — system-wide undo\n"
        << "\n"
        << "Usage:\n"
        << "  echo save [--description \"...\"]              Save current system state\n"
        << "  echo undo <seconds>                           Undo to N seconds ago\n"
        << "  echo undo --component <comp> <seconds>        Selective undo\n"
        << "    Components: filesystem, replay, process:<pid>\n"
        << "  echo redo                                     Redo last undo\n"
        << "  echo list                                     List saved states\n"
        << "  echo track <pid>                              Track process for undo\n"
        << "  echo untrack <pid>                            Stop tracking process\n"
        << "  echo auto <seconds>                           Set auto-save interval (0=off)\n";
}

static std::string format_time(std::chrono::system_clock::time_point tp) {
    auto tt = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
    localtime_r(&tt, &tm);
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

static std::string human_size(uint64_t bytes) {
    const char* units[] = {"B", "KiB", "MiB", "GiB"};
    int idx = 0;
    double val = static_cast<double>(bytes);
    while (val >= 1024.0 && idx < 3) {
        val /= 1024.0;
        ++idx;
    }
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1) << val << " " << units[idx];
    return ss.str();
}

static std::string time_ago(std::chrono::system_clock::time_point tp) {
    auto now = std::chrono::system_clock::now();
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now - tp).count();

    if (seconds < 60) return std::to_string(seconds) + "s ago";
    if (seconds < 3600) return std::to_string(seconds / 60) + "m ago";
    if (seconds < 86400) return std::to_string(seconds / 3600) + "h ago";
    return std::to_string(seconds / 86400) + "d ago";
}

static int cmd_save(const std::vector<std::string>& args) {
    std::string description;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--description" && i + 1 < args.size()) {
            description = args[++i];
        }
    }

    straylight::EchoEngine engine;
    auto result = engine.save_state(description);
    if (!result.has_value()) {
        std::cerr << "Error: " << result.error() << "\n";
        return 1;
    }

    const auto& state = result.value();
    std::cout << "State saved: " << state.state_id << "\n"
              << "  Snapshot:    " << (state.snapshot_id.empty() ? "none" : state.snapshot_id) << "\n"
              << "  Checkpoints: " << state.checkpoint_ids.size() << "\n"
              << "  Replay pos:  " << state.replay_position << "\n"
              << "  Size:        ~" << human_size(state.estimated_size_bytes) << "\n";

    return 0;
}

static int cmd_undo(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "Error: undo requires a time argument (seconds)\n";
        return 1;
    }

    std::string component;
    int seconds = 0;

    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--component" && i + 1 < args.size()) {
            component = args[++i];
        } else {
            // Try to parse as seconds
            try {
                seconds = std::stoi(args[i]);
            } catch (...) {
                std::cerr << "Error: invalid time argument: " << args[i] << "\n";
                return 1;
            }
        }
    }

    if (seconds <= 0) {
        std::cerr << "Error: seconds must be positive\n";
        return 1;
    }

    straylight::EchoEngine engine;

    straylight::Result<straylight::UndoReport, std::string> result =
        component.empty()
            ? engine.undo(seconds)
            : engine.undo_selective(component, seconds);

    if (!result.has_value()) {
        std::cerr << "Error: " << result.error() << "\n";
        return 1;
    }

    const auto& report = result.value();
    std::cout << "Undo complete (state " << report.state_id << "):\n";
    if (report.filesystem_restored) std::cout << "  Filesystem: restored\n";
    if (report.processes_restored > 0) {
        std::cout << "  Processes:  " << report.processes_restored << " restored\n";
    }
    if (report.replay_seeked) std::cout << "  Replay:     seeked\n";

    if (!report.details.empty()) {
        std::cout << "\nDetails:\n" << report.details;
    }

    return 0;
}

static int cmd_redo() {
    straylight::EchoEngine engine;
    auto result = engine.redo();
    if (!result.has_value()) {
        std::cerr << "Error: " << result.error() << "\n";
        return 1;
    }

    const auto& report = result.value();
    std::cout << "Redo complete:\n";
    if (!report.details.empty()) {
        std::cout << report.details;
    }

    return 0;
}

static int cmd_list() {
    straylight::EchoEngine engine;
    auto states = engine.list_states();

    if (states.empty()) {
        std::cout << "No saved states.\n";
        return 0;
    }

    std::cout << std::left
              << std::setw(24) << "STATE ID"
              << std::setw(22) << "TIMESTAMP"
              << std::setw(12) << "AGE"
              << std::setw(8)  << "PROCS"
              << std::setw(10) << "SIZE"
              << "DESCRIPTION"
              << "\n";
    std::cout << std::string(90, '-') << "\n";

    // Show newest first
    for (auto it = states.rbegin(); it != states.rend(); ++it) {
        const auto& s = *it;
        std::cout << std::left
                  << std::setw(24) << s.state_id
                  << std::setw(22) << format_time(s.timestamp)
                  << std::setw(12) << time_ago(s.timestamp)
                  << std::setw(8)  << s.checkpoint_ids.size()
                  << std::setw(10) << human_size(s.estimated_size_bytes)
                  << s.description
                  << "\n";
    }

    std::cout << "\n" << states.size() << " states saved.\n";

    // Show tracked processes
    auto tracked = engine.tracked_processes();
    if (!tracked.empty()) {
        std::cout << "Tracked processes:";
        for (pid_t pid : tracked) {
            std::cout << " " << pid;
        }
        std::cout << "\n";
    }

    return 0;
}

static int cmd_track(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "Error: track requires a PID argument\n";
        return 1;
    }

    pid_t pid = static_cast<pid_t>(std::stoi(args[0]));

    straylight::EchoEngine engine;
    engine.track_process(pid);

    std::cout << "Now tracking process " << pid << " for undo checkpoints.\n";
    return 0;
}

static int cmd_untrack(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "Error: untrack requires a PID argument\n";
        return 1;
    }

    pid_t pid = static_cast<pid_t>(std::stoi(args[0]));

    straylight::EchoEngine engine;
    engine.untrack_process(pid);

    std::cout << "Stopped tracking process " << pid << ".\n";
    return 0;
}

static int cmd_auto(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "Error: auto requires an interval (seconds, 0 to disable)\n";
        return 1;
    }

    int interval = std::stoi(args[0]);

    straylight::EchoEngine engine;
    engine.set_auto_save_interval(interval);

    if (interval > 0) {
        std::cout << "Auto-save enabled: every " << interval << " seconds.\n";
    } else {
        std::cout << "Auto-save disabled.\n";
    }

    return 0;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string command = argv[1];
    std::vector<std::string> args;
    for (int i = 2; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }

    if (command == "save")    return cmd_save(args);
    if (command == "undo")    return cmd_undo(args);
    if (command == "redo")    return cmd_redo();
    if (command == "list")    return cmd_list();
    if (command == "track")   return cmd_track(args);
    if (command == "untrack") return cmd_untrack(args);
    if (command == "auto")    return cmd_auto(args);

    if (command == "--help" || command == "-h") {
        print_usage();
        return 0;
    }

    std::cerr << "Unknown command: " << command << "\n";
    print_usage();
    return 1;
}
