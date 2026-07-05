// tools/audio-cli/main.cpp
// CLI front-end for straylight-audio — audio device and stream management.

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>

// This CLI tool communicates with the audio daemon via pactl/pw-cli commands
// directly, without linking the audio engine library. This allows it to work
// independently as a lightweight front-end.

static void print_usage() {
    std::cerr
        << "straylight-audio-cli — audio management CLI\n"
        << "\n"
        << "Usage:\n"
        << "  straylight-audio-cli devices                      List audio devices\n"
        << "  straylight-audio-cli streams                      List audio streams\n"
        << "  straylight-audio-cli volume <stream-id> <level>   Set stream volume (0-150%)\n"
        << "  straylight-audio-cli device-volume <id> <level>   Set device volume (0-150%)\n"
        << "  straylight-audio-cli default <in|out> <device-id> Set default device\n"
        << "  straylight-audio-cli mute <stream-id>             Toggle mute on stream\n"
        << "  straylight-audio-cli mute-device <device-id>      Toggle mute on device\n"
        << "  straylight-audio-cli route <from-id> <to-id>      Create audio loopback route\n"
        << "  straylight-audio-cli move <stream-id> <device-id> Move stream to device\n"
        << "  straylight-audio-cli profile <device-id> <name>   Set card profile\n";
}

static std::string run_cmd(const std::string& cmd) {
    std::array<char, 8192> buffer{};
    std::string output;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }
    pclose(pipe);
    return output;
}

static bool run_cmd_ok(const std::string& cmd) {
    int rc = std::system(cmd.c_str());
    return rc == 0;
}

// Format a volume percentage into a visual bar
static std::string volume_bar(int pct) {
    int filled = pct / 5;
    if (filled > 20) filled = 20;
    if (filled < 0) filled = 0;
    std::string bar = "[";
    for (int i = 0; i < 20; ++i) {
        bar += (i < filled) ? '#' : '-';
    }
    bar += "] " + std::to_string(pct) + "%";
    return bar;
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

    // -----------------------------------------------------------------------
    // devices
    // -----------------------------------------------------------------------
    if (command == "devices") {
        // List sinks
        std::cout << "=== Output Devices (Sinks) ===\n\n";
        std::string sinks = run_cmd("pactl list sinks short 2>/dev/null");
        if (sinks.empty()) {
            sinks = run_cmd("pw-cli ls Node 2>/dev/null");
        }

        // Use detailed listing for better output
        std::string detail = run_cmd("pactl list sinks 2>/dev/null");
        if (!detail.empty()) {
            std::istringstream stream(detail);
            std::string line;
            std::string id, name, desc, vol_str;
            bool muted = false;
            bool in_sink = false;

            auto print_sink = [&]() {
                if (id.empty()) return;
                std::cout << std::left << "  [" << std::setw(4) << id << "] "
                          << std::setw(40) << desc << " " << vol_str;
                if (muted) std::cout << " (MUTED)";
                std::cout << "\n";
                if (!name.empty()) std::cout << "         " << name << "\n";
            };

            while (std::getline(stream, line)) {
                auto pos = line.find_first_not_of(" \t");
                if (pos != std::string::npos) line = line.substr(pos);

                if (line.rfind("Sink #", 0) == 0) {
                    print_sink();
                    id = line.substr(6);
                    name.clear(); desc.clear(); vol_str.clear();
                    muted = false;
                    in_sink = true;
                } else if (in_sink) {
                    if (line.rfind("Name:", 0) == 0) name = line.substr(6);
                    else if (line.rfind("Description:", 0) == 0) desc = line.substr(13);
                    else if (line.rfind("Mute:", 0) == 0) muted = (line.find("yes") != std::string::npos);
                    else if (line.rfind("Volume:", 0) == 0) {
                        std::regex vol_re(R"((\d+)%)");
                        std::smatch m;
                        if (std::regex_search(line, m, vol_re)) {
                            vol_str = volume_bar(std::stoi(m[1].str()));
                        }
                    }
                }
            }
            print_sink();
        }

        // List sources
        std::cout << "\n=== Input Devices (Sources) ===\n\n";
        detail = run_cmd("pactl list sources 2>/dev/null");
        if (!detail.empty()) {
            std::istringstream stream(detail);
            std::string line;
            std::string id, name, desc, vol_str;
            bool muted = false;
            bool in_source = false;

            auto print_source = [&]() {
                if (id.empty()) return;
                // Skip monitors
                if (name.find(".monitor") != std::string::npos) return;
                std::cout << std::left << "  [" << std::setw(4) << id << "] "
                          << std::setw(40) << desc << " " << vol_str;
                if (muted) std::cout << " (MUTED)";
                std::cout << "\n";
            };

            while (std::getline(stream, line)) {
                auto pos = line.find_first_not_of(" \t");
                if (pos != std::string::npos) line = line.substr(pos);

                if (line.rfind("Source #", 0) == 0) {
                    print_source();
                    id = line.substr(8);
                    name.clear(); desc.clear(); vol_str.clear();
                    muted = false;
                    in_source = true;
                } else if (in_source) {
                    if (line.rfind("Name:", 0) == 0) name = line.substr(6);
                    else if (line.rfind("Description:", 0) == 0) desc = line.substr(13);
                    else if (line.rfind("Mute:", 0) == 0) muted = (line.find("yes") != std::string::npos);
                    else if (line.rfind("Volume:", 0) == 0) {
                        std::regex vol_re(R"((\d+)%)");
                        std::smatch m;
                        if (std::regex_search(line, m, vol_re)) {
                            vol_str = volume_bar(std::stoi(m[1].str()));
                        }
                    }
                }
            }
            print_source();
        }

        return 0;
    }

    // -----------------------------------------------------------------------
    // streams
    // -----------------------------------------------------------------------
    if (command == "streams") {
        std::cout << "=== Active Audio Streams ===\n\n";

        std::string detail = run_cmd("pactl list sink-inputs 2>/dev/null");
        if (!detail.empty()) {
            std::istringstream stream(detail);
            std::string line;
            std::string id, app, binary, vol_str, sink_id;
            bool muted = false;
            bool in_stream = false;

            auto print_stream = [&]() {
                if (id.empty()) return;
                std::string label = app.empty() ? binary : app;
                if (label.empty()) label = "(unknown)";
                std::cout << std::left << "  [" << std::setw(4) << id << "] "
                          << std::setw(30) << label << " -> sink " << sink_id
                          << "  " << vol_str;
                if (muted) std::cout << " (MUTED)";
                std::cout << "\n";
            };

            while (std::getline(stream, line)) {
                auto pos = line.find_first_not_of(" \t");
                if (pos != std::string::npos) line = line.substr(pos);

                if (line.rfind("Sink Input #", 0) == 0) {
                    print_stream();
                    id = line.substr(12);
                    app.clear(); binary.clear(); vol_str.clear(); sink_id.clear();
                    muted = false;
                    in_stream = true;
                } else if (in_stream) {
                    if (line.rfind("Sink:", 0) == 0) {
                        sink_id = line.substr(6);
                    } else if (line.rfind("Mute:", 0) == 0) {
                        muted = (line.find("yes") != std::string::npos);
                    } else if (line.rfind("Volume:", 0) == 0) {
                        std::regex vol_re(R"((\d+)%)");
                        std::smatch m;
                        if (std::regex_search(line, m, vol_re)) {
                            vol_str = volume_bar(std::stoi(m[1].str()));
                        }
                    } else if (line.find("application.name") != std::string::npos) {
                        std::regex name_re(R"rx(application\.name\s*=\s*"([^"]*)")rx");
                        std::smatch m;
                        if (std::regex_search(line, m, name_re)) app = m[1].str();
                    } else if (line.find("application.process.binary") != std::string::npos) {
                        std::regex bin_re(R"rx(application\.process\.binary\s*=\s*"([^"]*)")rx");
                        std::smatch m;
                        if (std::regex_search(line, m, bin_re)) binary = m[1].str();
                    }
                }
            }
            print_stream();
        }

        // Source outputs (recording streams)
        std::string rec = run_cmd("pactl list source-outputs 2>/dev/null");
        if (!rec.empty()) {
            bool printed_header = false;
            std::istringstream stream(rec);
            std::string line;
            std::string id, app;
            bool in_stream = false;

            auto print_rec = [&]() {
                if (id.empty()) return;
                if (!printed_header) {
                    std::cout << "\n=== Recording Streams ===\n\n";
                    printed_header = true;
                }
                std::cout << "  [" << id << "] " << (app.empty() ? "(unknown)" : app) << "\n";
            };

            while (std::getline(stream, line)) {
                auto pos = line.find_first_not_of(" \t");
                if (pos != std::string::npos) line = line.substr(pos);

                if (line.rfind("Source Output #", 0) == 0) {
                    print_rec();
                    id = line.substr(15);
                    app.clear();
                    in_stream = true;
                } else if (in_stream && line.find("application.name") != std::string::npos) {
                    std::regex name_re(R"rx(application\.name\s*=\s*"([^"]*)")rx");
                    std::smatch m;
                    if (std::regex_search(line, m, name_re)) app = m[1].str();
                }
            }
            print_rec();
        }

        return 0;
    }

    // -----------------------------------------------------------------------
    // volume <stream-id> <level>
    // -----------------------------------------------------------------------
    if (command == "volume") {
        if (argc < 4) {
            std::cerr << "Error: 'volume' requires stream-id and level\n";
            return 1;
        }
        std::string stream_id = argv[2];
        std::string level = argv[3];
        // Ensure level has % suffix
        if (level.back() != '%') level += "%";

        std::string cmd = "pactl set-sink-input-volume " + stream_id + " " + level + " 2>/dev/null";
        if (!run_cmd_ok(cmd)) {
            std::cerr << "Error: failed to set volume\n";
            return 1;
        }
        std::cout << "Stream " << stream_id << " volume set to " << level << "\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // device-volume <device-id> <level>
    // -----------------------------------------------------------------------
    if (command == "device-volume") {
        if (argc < 4) {
            std::cerr << "Error: 'device-volume' requires device-id and level\n";
            return 1;
        }
        std::string device_id = argv[2];
        std::string level = argv[3];
        if (level.back() != '%') level += "%";

        // Try sink first, then source
        std::string cmd = "pactl set-sink-volume " + device_id + " " + level + " 2>/dev/null";
        if (!run_cmd_ok(cmd)) {
            cmd = "pactl set-source-volume " + device_id + " " + level + " 2>/dev/null";
            if (!run_cmd_ok(cmd)) {
                std::cerr << "Error: failed to set device volume\n";
                return 1;
            }
        }
        std::cout << "Device " << device_id << " volume set to " << level << "\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // default <in|out> <device-id>
    // -----------------------------------------------------------------------
    if (command == "default") {
        if (argc < 4) {
            std::cerr << "Error: 'default' requires <in|out> and device-id\n";
            return 1;
        }
        std::string direction = argv[2];
        std::string device_id = argv[3];

        std::string target;
        if (direction == "out" || direction == "sink") target = "default-sink";
        else if (direction == "in" || direction == "source") target = "default-source";
        else {
            std::cerr << "Error: direction must be 'in' or 'out'\n";
            return 1;
        }

        std::string cmd = "pactl set-" + target + " " + device_id + " 2>/dev/null";
        if (!run_cmd_ok(cmd)) {
            std::cerr << "Error: failed to set default\n";
            return 1;
        }
        std::cout << "Default " << direction << "put device set to " << device_id << "\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // mute <stream-id>
    // -----------------------------------------------------------------------
    if (command == "mute") {
        if (argc < 3) {
            std::cerr << "Error: 'mute' requires a stream-id\n";
            return 1;
        }
        std::string stream_id = argv[2];
        std::string cmd = "pactl set-sink-input-mute " + stream_id + " toggle 2>/dev/null";
        if (!run_cmd_ok(cmd)) {
            std::cerr << "Error: failed to toggle mute\n";
            return 1;
        }
        std::cout << "Toggled mute on stream " << stream_id << "\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // mute-device <device-id>
    // -----------------------------------------------------------------------
    if (command == "mute-device") {
        if (argc < 3) {
            std::cerr << "Error: 'mute-device' requires a device-id\n";
            return 1;
        }
        std::string device_id = argv[2];
        std::string cmd = "pactl set-sink-mute " + device_id + " toggle 2>/dev/null";
        if (!run_cmd_ok(cmd)) {
            cmd = "pactl set-source-mute " + device_id + " toggle 2>/dev/null";
            if (!run_cmd_ok(cmd)) {
                std::cerr << "Error: failed to toggle mute\n";
                return 1;
            }
        }
        std::cout << "Toggled mute on device " << device_id << "\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // route <from-id> <to-id>
    // -----------------------------------------------------------------------
    if (command == "route") {
        if (argc < 4) {
            std::cerr << "Error: 'route' requires from-id and to-id\n";
            return 1;
        }
        std::string from = argv[2];
        std::string to = argv[3];

        // Create loopback
        std::string cmd = "pactl load-module module-loopback source=" + from +
                          " sink=" + to + " 2>/dev/null";
        std::string output = run_cmd(cmd);
        if (output.empty()) {
            // Try pw-loopback
            cmd = "pw-loopback --capture-props='target.object=" + from +
                  "' --playback-props='target.object=" + to + "' 2>/dev/null &";
            if (!run_cmd_ok(cmd)) {
                std::cerr << "Error: failed to create audio route\n";
                return 1;
            }
        }
        std::cout << "Audio route created: " << from << " -> " << to << "\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // move <stream-id> <device-id>
    // -----------------------------------------------------------------------
    if (command == "move") {
        if (argc < 4) {
            std::cerr << "Error: 'move' requires stream-id and device-id\n";
            return 1;
        }
        std::string stream_id = argv[2];
        std::string device_id = argv[3];

        std::string cmd = "pactl move-sink-input " + stream_id + " " +
                          device_id + " 2>/dev/null";
        if (!run_cmd_ok(cmd)) {
            cmd = "pactl move-source-output " + stream_id + " " +
                  device_id + " 2>/dev/null";
            if (!run_cmd_ok(cmd)) {
                std::cerr << "Error: failed to move stream\n";
                return 1;
            }
        }
        std::cout << "Stream " << stream_id << " moved to device " << device_id << "\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // profile <device-id> <profile-name>
    // -----------------------------------------------------------------------
    if (command == "profile") {
        if (argc < 4) {
            std::cerr << "Error: 'profile' requires device-id and profile name\n";
            return 1;
        }
        std::string device_id = argv[2];
        std::string profile_name = argv[3];

        std::string cmd = "pactl set-card-profile " + device_id + " " +
                          profile_name + " 2>/dev/null";
        if (!run_cmd_ok(cmd)) {
            std::cerr << "Error: failed to set card profile\n";
            return 1;
        }
        std::cout << "Card profile set to '" << profile_name << "'\n";
        return 0;
    }

    std::cerr << "Error: unknown command '" << command << "'\n";
    print_usage();
    return 1;
}
