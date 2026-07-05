// tools/replay-cli/main.cpp
// Command-line client for the StrayLight Replay flight recorder.
// Connects to /run/straylight/replay.sock and sends JSON-RPC requests.

#include <straylight/ipc_client.h>
#include <straylight/log.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

static void print_usage() {
    std::cerr
        << "Usage:\n"
        << "  straylight-replay last <duration>          Show events from last 5m/1h/etc\n"
        << "  straylight-replay crash                    Analyze most recent crash\n"
        << "  straylight-replay timeline [--from TS] [--to TS] [--type TYPE] [--pid PID]\n"
        << "  straylight-replay search <pattern>         Search event details\n"
        << "  straylight-replay export <file.json>       Export timeline as JSON\n"
        << "  straylight-replay status                   Show recorder stats\n"
        << "  straylight-replay watch [--type TYPE]      Live event stream\n"
        << "\n"
        << "Duration format: 5m, 1h, 30s, 2h30m\n";
}

static std::string default_socket() {
    const char* env = std::getenv("REPLAY_SOCKET");
    return env ? env : "/run/straylight/replay.sock";
}

/// Parse a duration string like "5m", "1h", "30s", "2h30m" into seconds.
static uint64_t parse_duration(const std::string& dur) {
    uint64_t total = 0;
    uint64_t current = 0;

    for (char c : dur) {
        if (c >= '0' && c <= '9') {
            current = current * 10 + static_cast<uint64_t>(c - '0');
        } else if (c == 'h' || c == 'H') {
            total += current * 3600;
            current = 0;
        } else if (c == 'm' || c == 'M') {
            total += current * 60;
            current = 0;
        } else if (c == 's' || c == 'S') {
            total += current;
            current = 0;
        }
    }

    // Bare number defaults to seconds
    total += current;

    // Default to 5 minutes if parse fails
    return (total > 0) ? total : 300;
}

static void pretty_print_json(const nlohmann::json& j, int indent = 0) {
    std::string pad(static_cast<size_t>(indent * 2), ' ');

    if (j.is_object()) {
        for (auto& [key, val] : j.items()) {
            if (val.is_string()) {
                std::string s = val.get<std::string>();
                if (s.find('\n') != std::string::npos) {
                    std::cout << pad << key << ":\n";
                    std::istringstream stream(s);
                    std::string line;
                    while (std::getline(stream, line)) {
                        std::cout << pad << "  " << line << "\n";
                    }
                } else {
                    std::cout << pad << key << ": " << s << "\n";
                }
            } else if (val.is_array()) {
                std::cout << pad << key << ":\n";
                for (const auto& item : val) {
                    if (item.is_object()) {
                        pretty_print_json(item, indent + 1);
                        std::cout << pad << "  ---\n";
                    } else {
                        std::cout << pad << "  - " << item.dump() << "\n";
                    }
                }
            } else if (val.is_object()) {
                std::cout << pad << key << ":\n";
                pretty_print_json(val, indent + 1);
            } else {
                std::cout << pad << key << ": " << val.dump() << "\n";
            }
        }
    } else if (j.is_string()) {
        // Multi-line string — print directly
        std::string s = j.get<std::string>();
        std::cout << s;
        if (!s.empty() && s.back() != '\n') std::cout << "\n";
    } else {
        std::cout << pad << j.dump(2) << "\n";
    }
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

    // Connect to the Replay daemon
    straylight::IpcJsonClient client;
    auto conn = client.connect(default_socket());
    if (!conn.has_value()) {
        std::cerr << "Error: could not connect to Replay daemon: " << conn.error() << "\n";
        std::cerr << "Is straylight-replay running?\n";
        return 1;
    }

    nlohmann::json request;
    request["jsonrpc"] = "2.0";
    request["id"] = 1;

    if (command == "status") {
        request["method"] = "status";

    } else if (command == "last") {
        request["method"] = "last";
        nlohmann::json params;
        if (argc >= 3) {
            params["seconds"] = parse_duration(argv[2]);
        } else {
            params["seconds"] = 300;  // Default 5 minutes
        }
        request["params"] = params;

    } else if (command == "crash") {
        request["method"] = "crash";

    } else if (command == "timeline") {
        request["method"] = "timeline";
        nlohmann::json params;
        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--from" && i + 1 < argc) {
                params["from_ns"] = std::strtoull(argv[++i], nullptr, 10);
            } else if (arg == "--to" && i + 1 < argc) {
                params["to_ns"] = std::strtoull(argv[++i], nullptr, 10);
            } else if (arg == "--type" && i + 1 < argc) {
                params["type"] = argv[++i];
            } else if (arg == "--pid" && i + 1 < argc) {
                params["pid"] = std::atoi(argv[++i]);
            }
        }
        request["params"] = params;

    } else if (command == "search") {
        if (argc < 3) {
            std::cerr << "Error: 'search' requires a pattern argument\n";
            return 1;
        }
        request["method"] = "search";
        nlohmann::json params;
        params["pattern"] = argv[2];
        request["params"] = params;

    } else if (command == "export") {
        if (argc < 3) {
            std::cerr << "Error: 'export' requires an output filename\n";
            return 1;
        }
        request["method"] = "export";
        nlohmann::json params;
        // Default export last 5 minutes
        uint64_t seconds = 300;
        std::string output_file = argv[2];

        for (int i = 3; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--last" && i + 1 < argc) {
                seconds = parse_duration(argv[++i]);
            }
        }
        params["seconds"] = seconds;
        request["params"] = params;

        // Send request, write result to file
        auto response = client.request(request);
        if (!response.has_value()) {
            std::cerr << "Error: " << response.error() << "\n";
            return 1;
        }

        const auto& resp = response.value();
        if (resp.contains("error")) {
            std::cerr << "Error: " << resp["error"].value("message", "unknown") << "\n";
            return 1;
        }

        if (resp.contains("result")) {
            std::string json_data;
            if (resp["result"].is_string()) {
                json_data = resp["result"].get<std::string>();
            } else {
                json_data = resp["result"].dump(2);
            }

            std::ofstream out(output_file);
            if (!out.is_open()) {
                std::cerr << "Error: cannot open " << output_file << " for writing\n";
                return 1;
            }
            out << json_data;
            out.close();
            std::cout << "Exported to " << output_file << "\n";
        }
        return 0;

    } else if (command == "watch") {
        // Poll the daemon repeatedly for new events
        std::string type_filter;
        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--type" && i + 1 < argc) {
                type_filter = argv[++i];
            }
        }

        std::cout << "Watching events (Ctrl+C to stop)...\n\n";

        uint64_t last_seen_ts = 0;
        while (true) {
            nlohmann::json watch_req;
            watch_req["jsonrpc"] = "2.0";
            watch_req["id"] = 1;
            watch_req["method"] = "watch";

            auto response = client.request(watch_req);
            if (!response.has_value()) {
                std::cerr << "Connection lost: " << response.error() << "\n";
                return 1;
            }

            const auto& resp = response.value();
            if (resp.contains("result") && resp["result"].is_array()) {
                for (const auto& ev : resp["result"]) {
                    uint64_t ts = ev.value("timestamp_ns", uint64_t(0));
                    if (ts <= last_seen_ts) continue;

                    std::string type = ev.value("type", "");
                    if (!type_filter.empty() && type != type_filter) continue;

                    std::string proc = ev.value("process_name", "");
                    uint32_t pid = ev.value("pid", 0u);
                    std::string detail = ev.value("detail", "");

                    std::cout << "[" << type << "]";
                    if (!proc.empty()) {
                        std::cout << " " << proc;
                        if (pid > 0) std::cout << ":" << pid;
                    }
                    if (!detail.empty()) {
                        if (detail.size() > 100) detail = detail.substr(0, 97) + "...";
                        std::cout << " " << detail;
                    }
                    std::cout << "\n";

                    last_seen_ts = ts;
                }
            }

            // Reconnect for next poll (daemon closes connection after each request)
            client.disconnect();
            auto reconn = client.connect(default_socket());
            if (!reconn.has_value()) {
                std::cerr << "Reconnection failed: " << reconn.error() << "\n";
                return 1;
            }

            // Poll interval
            usleep(500000);  // 500ms
        }

    } else {
        std::cerr << "Error: unknown command '" << command << "'\n";
        print_usage();
        return 1;
    }

    // Send request and print response
    auto response = client.request(request);
    if (!response.has_value()) {
        std::cerr << "Error: " << response.error() << "\n";
        return 1;
    }

    const auto& resp = response.value();

    if (resp.contains("error")) {
        const auto& err = resp["error"];
        std::cerr << "Error";
        if (err.contains("code")) {
            std::cerr << " (" << err["code"].dump() << ")";
        }
        if (err.contains("message")) {
            std::cerr << ": " << err["message"].get<std::string>();
        }
        std::cerr << "\n";
        return 1;
    }

    if (resp.contains("result")) {
        const auto& result = resp["result"];
        if (result.is_string()) {
            // Try to parse as JSON first
            try {
                auto parsed = nlohmann::json::parse(result.get<std::string>());
                pretty_print_json(parsed);
            } catch (...) {
                // Plain text result
                std::cout << result.get<std::string>();
                std::string s = result.get<std::string>();
                if (!s.empty() && s.back() != '\n') std::cout << "\n";
            }
        } else {
            pretty_print_json(result);
        }
    }

    return 0;
}
