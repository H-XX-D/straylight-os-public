// tools/flux-cli/main.cpp
// Command-line client for the Flux stream processor daemon.
// Connects to /run/straylight/flux.sock and sends JSON-RPC requests.

#include <straylight/ipc_client.h>
#include <straylight/log.h>

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

static void print_usage() {
    std::cerr
        << "Usage:\n"
        << "  straylight-flux create <name> [--buffer N]            Create a named stream\n"
        << "  straylight-flux delete <name>                         Delete a stream\n"
        << "  straylight-flux list                                  List all streams\n"
        << "  straylight-flux info <name>                           Show stream info\n"
        << "  straylight-flux publish <name> '<json>'               Publish event\n"
        << "  straylight-flux subscribe <name> [--filter EXPR]      Subscribe (prints events)\n"
        << "  straylight-flux replay <name> [--count N]             Replay last N events\n"
        << "  straylight-flux transform <name> --jq <path>          Extract field from events\n"
        << "  straylight-flux aggregate <name> --field F --type T   Aggregate over window\n"
        << "  straylight-flux pipe <name>                           Pipe events to stdout\n";
}

static std::string default_socket() {
    const char* env = std::getenv("FLUX_SOCKET");
    return env ? env : "/run/straylight/flux.sock";
}

static void pretty_print_json(const nlohmann::json& j, int indent = 0) {
    std::string pad(static_cast<size_t>(indent * 2), ' ');

    if (j.is_object()) {
        for (auto& [key, val] : j.items()) {
            if (val.is_string()) {
                std::cout << pad << key << ": " << val.get<std::string>() << "\n";
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
    } else if (j.is_array()) {
        for (const auto& item : j) {
            if (item.is_object()) {
                pretty_print_json(item, indent);
                std::cout << pad << "---\n";
            } else {
                std::cout << pad << item.dump() << "\n";
            }
        }
    } else {
        std::cout << pad << j.dump(2) << "\n";
    }
}

static int send_request(straylight::IpcJsonClient& client,
                        const nlohmann::json& request) {
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
                pretty_print_json(parsed);
            } catch (...) {
                std::cout << result.get<std::string>() << "\n";
            }
        } else {
            pretty_print_json(result);
        }
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
        std::cerr << "Error: could not connect to Flux daemon: " << conn.error() << "\n";
        std::cerr << "Is straylight-flux running?\n";
        return 1;
    }

    nlohmann::json request;
    request["jsonrpc"] = "2.0";
    request["id"] = 1;

    if (command == "create") {
        if (argc < 3) {
            std::cerr << "Error: 'create' requires a stream name\n";
            return 1;
        }
        request["method"] = "create";
        nlohmann::json params;
        params["name"] = argv[2];

        size_t buffer = 1000;
        for (int i = 3; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--buffer" && i + 1 < argc) {
                buffer = static_cast<size_t>(std::atol(argv[++i]));
            }
        }
        params["buffer"] = buffer;
        request["params"] = params;

    } else if (command == "delete") {
        if (argc < 3) {
            std::cerr << "Error: 'delete' requires a stream name\n";
            return 1;
        }
        request["method"] = "delete";
        request["params"] = {{"name", argv[2]}};

    } else if (command == "list") {
        request["method"] = "list";

    } else if (command == "info") {
        if (argc < 3) {
            std::cerr << "Error: 'info' requires a stream name\n";
            return 1;
        }
        request["method"] = "info";
        request["params"] = {{"name", argv[2]}};

    } else if (command == "publish") {
        if (argc < 4) {
            std::cerr << "Error: 'publish' requires a stream name and JSON payload\n";
            return 1;
        }
        request["method"] = "publish";
        nlohmann::json params;
        params["stream"] = argv[2];
        try {
            params["payload"] = nlohmann::json::parse(argv[3]);
        } catch (const nlohmann::json::parse_error& e) {
            std::cerr << "Error: invalid JSON payload: " << e.what() << "\n";
            return 1;
        }
        request["params"] = params;

    } else if (command == "subscribe" || command == "pipe") {
        if (argc < 3) {
            std::cerr << "Error: '" << command << "' requires a stream name\n";
            return 1;
        }
        std::string stream_name = argv[2];
        std::string filter;
        for (int i = 3; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--filter" && i + 1 < argc) {
                filter = argv[++i];
            }
        }

        // For subscribe/pipe, we poll replay in a loop
        // (Real implementation would use a persistent subscription via shared memory or websocket)
        uint64_t last_seq = 0;

        // Get initial state
        request["method"] = "replay";
        request["params"] = {{"stream", stream_name}, {"count", 0}};
        auto r = client.request(request);
        if (r.has_value() && r.value().contains("result") && r.value()["result"].is_array()) {
            auto& events = r.value()["result"];
            if (!events.empty()) {
                last_seq = events.back().value("sequence", static_cast<uint64_t>(0));
            }
        }

        // Poll loop
        while (true) {
            request["method"] = "replay";
            request["params"] = {{"stream", stream_name}, {"count", 100}};
            auto resp = client.request(request);
            if (!resp.has_value()) {
                std::cerr << "Connection lost: " << resp.error() << "\n";
                return 1;
            }

            if (resp.value().contains("result") && resp.value()["result"].is_array()) {
                for (const auto& ev : resp.value()["result"]) {
                    uint64_t seq = ev.value("sequence", static_cast<uint64_t>(0));
                    if (seq <= last_seq) continue;
                    last_seq = seq;

                    auto payload = ev.value("payload", nlohmann::json::object());

                    // Apply client-side filter if specified
                    if (!filter.empty()) {
                        // Simple client-side filter evaluation
                        bool pass = true;
                        // Parse "field > value" style filter
                        auto gt_pos = filter.find(" > ");
                        auto lt_pos = filter.find(" < ");
                        auto eq_pos = filter.find(" == ");

                        if (gt_pos != std::string::npos) {
                            std::string field = filter.substr(0, gt_pos);
                            std::string val_str = filter.substr(gt_pos + 3);
                            if (payload.contains(field) && payload[field].is_number()) {
                                try {
                                    double threshold = std::stod(val_str);
                                    pass = payload[field].get<double>() > threshold;
                                } catch (...) { pass = false; }
                            } else {
                                pass = false;
                            }
                        } else if (lt_pos != std::string::npos) {
                            std::string field = filter.substr(0, lt_pos);
                            std::string val_str = filter.substr(lt_pos + 3);
                            if (payload.contains(field) && payload[field].is_number()) {
                                try {
                                    double threshold = std::stod(val_str);
                                    pass = payload[field].get<double>() < threshold;
                                } catch (...) { pass = false; }
                            } else {
                                pass = false;
                            }
                        } else if (eq_pos != std::string::npos) {
                            std::string field = filter.substr(0, eq_pos);
                            std::string val_str = filter.substr(eq_pos + 4);
                            if (payload.contains(field)) {
                                if (payload[field].is_string()) {
                                    pass = payload[field].get<std::string>() == val_str;
                                } else {
                                    pass = payload[field].dump() == val_str;
                                }
                            } else {
                                pass = false;
                            }
                        }

                        if (!pass) continue;
                    }

                    if (command == "pipe") {
                        std::cout << payload.dump() << "\n";
                    } else {
                        std::cout << "[" << seq << "] " << payload.dump() << "\n";
                    }
                    std::cout.flush();
                }
            }

            // Poll interval
            usleep(100000); // 100ms
        }

    } else if (command == "replay") {
        if (argc < 3) {
            std::cerr << "Error: 'replay' requires a stream name\n";
            return 1;
        }
        request["method"] = "replay";
        nlohmann::json params;
        params["stream"] = argv[2];
        size_t count = 10;
        for (int i = 3; i < argc; ++i) {
            std::string arg = argv[i];
            if ((arg == "--count" || arg == "-n") && i + 1 < argc) {
                count = static_cast<size_t>(std::atol(argv[++i]));
            }
        }
        params["count"] = count;
        request["params"] = params;

    } else if (command == "transform") {
        if (argc < 3) {
            std::cerr << "Error: 'transform' requires a stream name\n";
            return 1;
        }
        request["method"] = "transform";
        nlohmann::json params;
        params["stream"] = argv[2];
        std::string path;
        size_t count = 10;
        for (int i = 3; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--jq" && i + 1 < argc) {
                path = argv[++i];
            } else if ((arg == "--count" || arg == "-n") && i + 1 < argc) {
                count = static_cast<size_t>(std::atol(argv[++i]));
            }
        }
        if (path.empty()) {
            std::cerr << "Error: 'transform' requires --jq <path>\n";
            return 1;
        }
        params["path"] = path;
        params["count"] = count;
        request["params"] = params;

    } else if (command == "aggregate") {
        if (argc < 3) {
            std::cerr << "Error: 'aggregate' requires a stream name\n";
            return 1;
        }
        request["method"] = "aggregate";
        nlohmann::json params;
        params["stream"] = argv[2];
        std::string field;
        std::string type = "avg";
        size_t window = 10;
        for (int i = 3; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--field" && i + 1 < argc) {
                field = argv[++i];
            } else if (arg == "--type" && i + 1 < argc) {
                type = argv[++i];
            } else if (arg == "--window" && i + 1 < argc) {
                window = static_cast<size_t>(std::atol(argv[++i]));
            }
        }
        if (field.empty()) {
            std::cerr << "Error: 'aggregate' requires --field <field>\n";
            return 1;
        }
        params["field"] = field;
        params["type"] = type;
        params["window"] = window;
        request["params"] = params;

    } else {
        std::cerr << "Error: unknown command '" << command << "'\n";
        print_usage();
        return 1;
    }

    return send_request(client, request);
}
