// tools/alice-cli/main.cpp
// Command-line client for the Alice system monitor daemon.
// Connects to /run/straylight/alice.sock and sends JSON-RPC requests.

#include <straylight/ipc_client.h>
#include <straylight/log.h>

#include <cstdlib>
#include <iostream>
#include <string>

static void print_usage() {
    std::cerr << "Usage:\n"
              << "  straylight-alice status              Quick health check\n"
              << "  straylight-alice ask \"question\"       Ask Alice a question\n"
              << "  straylight-alice analyze              Full system analysis\n"
              << "  straylight-alice alerts [--count N]   View recent alerts\n"
              << "  straylight-alice logs [--limit N]     View analyzed logs\n";
}

static std::string default_socket() {
    const char* env = std::getenv("ALICE_SOCKET");
    return env ? env : "/run/straylight/alice.sock";
}

static void pretty_print_json(const nlohmann::json& j, int indent = 0) {
    std::string pad(static_cast<size_t>(indent * 2), ' ');

    if (j.is_object()) {
        for (auto& [key, val] : j.items()) {
            if (val.is_string()) {
                std::string s = val.get<std::string>();
                // Multi-line strings get special formatting
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

    // Connect to Alice daemon
    straylight::IpcJsonClient client;
    auto conn = client.connect(default_socket());
    if (!conn.has_value()) {
        std::cerr << "Error: could not connect to Alice daemon: " << conn.error() << "\n";
        std::cerr << "Is straylight-alice running?\n";
        return 1;
    }

    nlohmann::json request;
    request["jsonrpc"] = "2.0";
    request["id"] = 1;

    if (command == "status") {
        request["method"] = "status";
    } else if (command == "ask") {
        if (argc < 3) {
            std::cerr << "Error: 'ask' requires a question argument\n";
            return 1;
        }
        request["method"] = "ask";
        nlohmann::json params;
        params["query"] = argv[2];
        request["params"] = params;
    } else if (command == "analyze") {
        request["method"] = "analyze";
    } else if (command == "alerts") {
        request["method"] = "alerts";
        nlohmann::json params;
        int count = 20;
        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if ((arg == "--count" || arg == "-n") && i + 1 < argc) {
                count = std::atoi(argv[++i]);
            }
        }
        params["count"] = count;
        request["params"] = params;
    } else if (command == "logs") {
        request["method"] = "logs";
        nlohmann::json params;
        int limit = 50;
        for (int i = 2; i < argc; ++i) {
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

    // Check for JSON-RPC error
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

    // Pretty-print the result
    if (resp.contains("result")) {
        const auto& result = resp["result"];
        // Result is a JSON string that needs to be parsed
        if (result.is_string()) {
            try {
                auto parsed = nlohmann::json::parse(result.get<std::string>());
                pretty_print_json(parsed);
            } catch (...) {
                std::cout << result.get<std::string>() << "\n";
            }
        } else {
            pretty_print_json(result);
        }
    } else {
        // Fallback: print raw response
        pretty_print_json(resp);
    }

    return 0;
}
