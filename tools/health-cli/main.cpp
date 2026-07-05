// tools/health-cli/main.cpp
// Command-line client for the StrayLight Health daemon.
// Connects to /run/straylight/health.sock and sends JSON-RPC requests.

#include <straylight/ipc_client.h>
#include <straylight/log.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

static void print_usage() {
    std::cerr
        << "straylight-health — System health dashboard CLI\n\n"
        << "Usage:\n"
        << "  straylight-health status               Show health score + breakdown\n"
        << "  straylight-health watch                 Live updating health display\n"
        << "  straylight-health report [--output FILE]  Generate detailed HTML report\n"
        << "  straylight-health history [--limit N]   Show score history\n"
        << "  straylight-health check <name>          Run a single check\n";
}

static std::string default_socket() {
    const char* env = std::getenv("HEALTH_SOCKET");
    return env ? env : "/run/straylight/health.sock";
}

static void pretty_print(const nlohmann::json& j, int indent = 0) {
    std::string pad(static_cast<size_t>(indent * 2), ' ');

    if (j.is_object()) {
        for (auto& [key, val] : j.items()) {
            if (val.is_string()) {
                std::cout << pad << key << ": " << val.get<std::string>() << "\n";
            } else if (val.is_array()) {
                std::cout << pad << key << ":\n";
                for (const auto& item : val) {
                    if (item.is_object()) {
                        pretty_print(item, indent + 1);
                        std::cout << pad << "  ---\n";
                    } else {
                        std::cout << pad << "  - " << item.dump() << "\n";
                    }
                }
            } else if (val.is_object()) {
                std::cout << pad << key << ":\n";
                pretty_print(val, indent + 1);
            } else {
                std::cout << pad << key << ": " << val.dump() << "\n";
            }
        }
    } else {
        std::cout << pad << j.dump(2) << "\n";
    }
}

static int cmd_status(straylight::IpcJsonClient& client) {
    nlohmann::json request;
    request["jsonrpc"] = "2.0";
    request["id"] = 1;
    request["method"] = "status";

    auto response = client.request(request);
    if (!response.has_value()) {
        std::cerr << "Error: " << response.error() << "\n";
        return 1;
    }

    const auto& resp = response.value();
    if (resp.contains("error")) {
        std::cerr << "Error: " << resp["error"].dump() << "\n";
        return 1;
    }

    if (resp.contains("result")) {
        const auto& result = resp["result"];
        if (result.is_string()) {
            std::cout << result.get<std::string>() << "\n";
        } else {
            pretty_print(result);
        }
    }

    return 0;
}

static int cmd_watch(straylight::IpcJsonClient& client) {
    std::cout << "Watching health status (Ctrl+C to stop)...\n\n";

    while (true) {
        nlohmann::json request;
        request["jsonrpc"] = "2.0";
        request["id"] = 1;
        request["method"] = "status";

        auto response = client.request(request);
        if (!response.has_value()) {
            std::cerr << "Connection lost: " << response.error() << "\n";
            return 1;
        }

        // Clear screen
        std::cout << "\033[2J\033[H";

        const auto& resp = response.value();
        if (resp.contains("result")) {
            const auto& result = resp["result"];
            if (result.is_string()) {
                std::cout << result.get<std::string>();
            } else {
                pretty_print(result);
            }
        }

        std::cout.flush();
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
}

static int cmd_report(straylight::IpcJsonClient& client,
                      const std::string& output_path) {
    nlohmann::json request;
    request["jsonrpc"] = "2.0";
    request["id"] = 1;
    request["method"] = "report";

    auto response = client.request(request);
    if (!response.has_value()) {
        std::cerr << "Error: " << response.error() << "\n";
        return 1;
    }

    const auto& resp = response.value();
    if (resp.contains("error")) {
        std::cerr << "Error: " << resp["error"].dump() << "\n";
        return 1;
    }

    std::string html;
    if (resp.contains("result")) {
        const auto& result = resp["result"];
        if (result.is_string()) {
            html = result.get<std::string>();
        } else {
            html = result.dump(2);
        }
    }

    if (output_path.empty()) {
        std::cout << html;
    } else {
        std::ofstream out(output_path, std::ios::trunc);
        if (!out) {
            std::cerr << "Error: cannot write to " << output_path << "\n";
            return 1;
        }
        out << html;
        std::cout << "Report written to " << output_path << "\n";
    }

    return 0;
}

static int cmd_history(straylight::IpcJsonClient& client, int limit) {
    nlohmann::json request;
    request["jsonrpc"] = "2.0";
    request["id"] = 1;
    request["method"] = "history";
    nlohmann::json params;
    params["limit"] = limit;
    request["params"] = params;

    auto response = client.request(request);
    if (!response.has_value()) {
        std::cerr << "Error: " << response.error() << "\n";
        return 1;
    }

    const auto& resp = response.value();
    if (resp.contains("error")) {
        std::cerr << "Error: " << resp["error"].dump() << "\n";
        return 1;
    }

    if (resp.contains("result")) {
        pretty_print(resp["result"]);
    }

    return 0;
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

    straylight::IpcJsonClient client;
    auto conn = client.connect(default_socket());
    if (!conn.has_value()) {
        std::cerr << "Error: could not connect to health daemon: " << conn.error() << "\n";
        std::cerr << "Is straylight-health running?\n";
        return 1;
    }

    if (command == "status") {
        return cmd_status(client);
    } else if (command == "watch") {
        return cmd_watch(client);
    } else if (command == "report") {
        std::string output;
        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if ((arg == "--output" || arg == "-o") && i + 1 < argc) {
                output = argv[++i];
            }
        }
        return cmd_report(client, output);
    } else if (command == "history") {
        int limit = 20;
        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if ((arg == "--limit" || arg == "-n") && i + 1 < argc) {
                limit = std::atoi(argv[++i]);
            }
        }
        return cmd_history(client, limit);
    } else {
        std::cerr << "Error: unknown command '" << command << "'\n";
        print_usage();
        return 1;
    }
}
