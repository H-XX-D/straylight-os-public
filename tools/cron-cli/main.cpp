// tools/cron-cli/main.cpp
// Command-line client for the StrayLight Cron daemon.
// Connects to /run/straylight/cron.sock and sends JSON-RPC requests.

#include <straylight/ipc_client.h>
#include <straylight/log.h>

#include <cstdlib>
#include <iostream>
#include <string>

static void print_usage() {
    std::cerr
        << "straylight-cron — Smart task scheduler CLI\n\n"
        << "Usage:\n"
        << "  straylight-cron add --name <name> --schedule <spec> --cmd <command>\n"
        << "  straylight-cron list                     List all tasks\n"
        << "  straylight-cron status [name]            Show task status\n"
        << "  straylight-cron run <name>               Run a task immediately\n"
        << "  straylight-cron enable <name>            Enable a task\n"
        << "  straylight-cron disable <name>           Disable a task\n"
        << "  straylight-cron remove <name>            Remove a task\n"
        << "  straylight-cron history <name> [--limit N]  Show run history\n"
        << "\nSchedule formats:\n"
        << "  'every 30s'  'every 5m'  'every 1h'  'every 1d'\n"
        << "  'daily 03:00'  'hourly'  'weekly'\n"
        << "\nOptions:\n"
        << "  --depends <task>         Add dependency\n"
        << "  --retries <N>            Max retry count\n"
        << "  --max-cpu <percent>      Resource constraint\n"
        << "  --min-memory <MB>        Resource constraint\n"
        << "  --missed-policy <skip|run>  Catch-up policy\n";
}

static std::string default_socket() {
    const char* env = std::getenv("CRON_SOCKET");
    return env ? env : "/run/straylight/cron.sock";
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

    // Connect to daemon
    straylight::IpcJsonClient client;
    auto conn = client.connect(default_socket());
    if (!conn.has_value()) {
        std::cerr << "Error: could not connect to cron daemon: " << conn.error() << "\n";
        std::cerr << "Is straylight-cron running?\n";
        return 1;
    }

    nlohmann::json request;
    request["jsonrpc"] = "2.0";
    request["id"] = 1;

    if (command == "add") {
        request["method"] = "add";
        nlohmann::json params;

        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if ((arg == "--name" || arg == "-n") && i + 1 < argc) {
                params["name"] = argv[++i];
            } else if ((arg == "--schedule" || arg == "-s") && i + 1 < argc) {
                params["schedule"] = argv[++i];
            } else if ((arg == "--cmd" || arg == "-c") && i + 1 < argc) {
                params["command"] = argv[++i];
            } else if (arg == "--depends" && i + 1 < argc) {
                if (!params.contains("depends_on")) {
                    params["depends_on"] = nlohmann::json::array();
                }
                params["depends_on"].push_back(argv[++i]);
            } else if (arg == "--retries" && i + 1 < argc) {
                params["max_retries"] = std::atoi(argv[++i]);
            } else if (arg == "--max-cpu" && i + 1 < argc) {
                params["max_cpu_percent"] = std::atof(argv[++i]);
            } else if (arg == "--min-memory" && i + 1 < argc) {
                params["min_free_memory_mb"] = std::atoi(argv[++i]);
            } else if (arg == "--missed-policy" && i + 1 < argc) {
                params["missed_policy"] = argv[++i];
            }
        }

        if (!params.contains("name") || !params.contains("schedule") || !params.contains("command")) {
            std::cerr << "Error: --name, --schedule, and --cmd are required\n";
            return 1;
        }
        request["params"] = params;

    } else if (command == "list") {
        request["method"] = "list";

    } else if (command == "status") {
        request["method"] = "status";
        if (argc >= 3) {
            nlohmann::json params;
            params["name"] = argv[2];
            request["params"] = params;
        }

    } else if (command == "run") {
        if (argc < 3) {
            std::cerr << "Usage: straylight-cron run <name>\n";
            return 1;
        }
        request["method"] = "run";
        nlohmann::json params;
        params["name"] = argv[2];
        request["params"] = params;

    } else if (command == "enable") {
        if (argc < 3) {
            std::cerr << "Usage: straylight-cron enable <name>\n";
            return 1;
        }
        request["method"] = "enable";
        nlohmann::json params;
        params["name"] = argv[2];
        request["params"] = params;

    } else if (command == "disable") {
        if (argc < 3) {
            std::cerr << "Usage: straylight-cron disable <name>\n";
            return 1;
        }
        request["method"] = "disable";
        nlohmann::json params;
        params["name"] = argv[2];
        request["params"] = params;

    } else if (command == "remove") {
        if (argc < 3) {
            std::cerr << "Usage: straylight-cron remove <name>\n";
            return 1;
        }
        request["method"] = "remove";
        nlohmann::json params;
        params["name"] = argv[2];
        request["params"] = params;

    } else if (command == "history") {
        if (argc < 3) {
            std::cerr << "Usage: straylight-cron history <name> [--limit N]\n";
            return 1;
        }
        request["method"] = "history";
        nlohmann::json params;
        params["name"] = argv[2];
        int limit = 20;
        for (int i = 3; i < argc; ++i) {
            std::string arg = argv[i];
            if ((arg == "--limit" || arg == "-n") && i + 1 < argc) {
                limit = std::atoi(argv[++i]);
            }
        }
        params["limit"] = limit;
        request["params"] = params;

    } else {
        std::cerr << "Error: unknown command '" << command << "'\n";
        print_usage();
        return 1;
    }

    auto response = client.request(request);
    if (!response.has_value()) {
        std::cerr << "Error: " << response.error() << "\n";
        return 1;
    }

    const auto& resp = response.value();

    if (resp.contains("error")) {
        const auto& err = resp["error"];
        std::cerr << "Error";
        if (err.contains("code")) std::cerr << " (" << err["code"].dump() << ")";
        if (err.contains("message")) std::cerr << ": " << err["message"].get<std::string>();
        std::cerr << "\n";
        return 1;
    }

    if (resp.contains("result")) {
        const auto& result = resp["result"];
        if (result.is_string()) {
            try {
                auto parsed = nlohmann::json::parse(result.get<std::string>());
                pretty_print(parsed);
            } catch (...) {
                std::cout << result.get<std::string>() << "\n";
            }
        } else {
            pretty_print(result);
        }
    } else {
        pretty_print(resp);
    }

    return 0;
}
