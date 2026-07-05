// tools/weave-cli/main.cpp
// CLI for straylight-weave — dynamic service composition.

#include <straylight/ipc_client.h>

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

static const char* WEAVE_SOCKET = "/run/straylight/weave.sock";

static void print_usage() {
    std::cerr
        << "straylight-weave-cli — dynamic service composition\n"
        << "\n"
        << "Usage:\n"
        << "  weave create <name> \"<pipeline-spec>\"   Create pipeline from DSL\n"
        << "  weave start <name>                       Start a pipeline\n"
        << "  weave stop <name>                        Stop a pipeline\n"
        << "  weave delete <name>                      Delete a pipeline\n"
        << "  weave list                               List all pipelines\n"
        << "  weave status <name>                      Show pipeline details\n"
        << "  weave metrics <name>                     Show per-node metrics\n"
        << "  weave nodes                              List available node types\n"
        << "\n"
        << "Pipeline DSL:\n"
        << "  Nodes are separated by '|' with optional --key=value arguments:\n"
        << "  \"camera-capture | vpu-encode --codec=h265 | mesh-broadcast --group=renders\"\n";
}

static straylight::Result<nlohmann::json, std::string> rpc_call(
    const std::string& method,
    const nlohmann::json& params = nlohmann::json::object()) {
    straylight::IpcJsonClient client;
    auto conn = client.connect(WEAVE_SOCKET);
    if (!conn.has_value()) {
        return straylight::Result<nlohmann::json, std::string>::error(
            "Cannot connect to weave daemon: " + conn.error());
    }

    nlohmann::json req;
    req["jsonrpc"] = "2.0";
    req["method"] = method;
    req["params"] = params;
    req["id"] = 1;

    auto resp = client.request(req);
    if (!resp.has_value()) {
        return straylight::Result<nlohmann::json, std::string>::error(resp.error());
    }

    auto& j = resp.value();
    if (j.contains("error")) {
        return straylight::Result<nlohmann::json, std::string>::error(
            j["error"].value("message", "Unknown error"));
    }

    return straylight::Result<nlohmann::json, std::string>::ok(j["result"]);
}

static int cmd_create(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        std::cerr << "Usage: weave create <name> \"<pipeline-spec>\"\n";
        return 1;
    }

    std::string name = args[0];

    // Join remaining args as the spec (allows quoting flexibility)
    std::string spec;
    for (size_t i = 1; i < args.size(); ++i) {
        if (!spec.empty()) spec += " ";
        spec += args[i];
    }

    nlohmann::json params;
    params["name"] = name;
    params["spec"] = spec;

    auto result = rpc_call("create", params);
    if (!result.has_value()) {
        std::cerr << "Error: " << result.error() << "\n";
        return 1;
    }

    std::cout << "Pipeline '" << name << "' created.\n";
    return 0;
}

static int cmd_start(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "Usage: weave start <name>\n";
        return 1;
    }

    nlohmann::json params;
    params["name"] = args[0];

    auto result = rpc_call("start", params);
    if (!result.has_value()) {
        std::cerr << "Error: " << result.error() << "\n";
        return 1;
    }

    std::cout << "Pipeline '" << args[0] << "' started.\n";
    return 0;
}

static int cmd_stop(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "Usage: weave stop <name>\n";
        return 1;
    }

    nlohmann::json params;
    params["name"] = args[0];

    auto result = rpc_call("stop", params);
    if (!result.has_value()) {
        std::cerr << "Error: " << result.error() << "\n";
        return 1;
    }

    std::cout << "Pipeline '" << args[0] << "' stopped.\n";
    return 0;
}

static int cmd_delete(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "Usage: weave delete <name>\n";
        return 1;
    }

    nlohmann::json params;
    params["name"] = args[0];

    auto result = rpc_call("delete", params);
    if (!result.has_value()) {
        std::cerr << "Error: " << result.error() << "\n";
        return 1;
    }

    std::cout << "Pipeline '" << args[0] << "' deleted.\n";
    return 0;
}

static int cmd_list() {
    auto result = rpc_call("list");
    if (!result.has_value()) {
        std::cerr << "Error: " << result.error() << "\n";
        return 1;
    }

    const auto& pipelines = result.value();
    if (!pipelines.is_array() || pipelines.empty()) {
        std::cout << "No pipelines.\n";
        return 0;
    }

    std::cout << std::left
              << std::setw(24) << "NAME"
              << std::setw(12) << "STATE"
              << "\n";
    std::cout << std::string(36, '-') << "\n";

    for (const auto& p : pipelines) {
        std::string state = p.value("state", "unknown");
        std::cout << std::left
                  << std::setw(24) << p.value("name", "?")
                  << std::setw(12) << state
                  << "\n";
    }

    return 0;
}

static int cmd_status(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "Usage: weave status <name>\n";
        return 1;
    }

    nlohmann::json params;
    params["name"] = args[0];

    auto result = rpc_call("status", params);
    if (!result.has_value()) {
        std::cerr << "Error: " << result.error() << "\n";
        return 1;
    }

    const auto& status = result.value();

    std::cout << "Pipeline: " << status.value("name", "?") << "\n"
              << "State:    " << status.value("state", "unknown") << "\n";

    if (status.contains("uptime_seconds")) {
        auto secs = status["uptime_seconds"].get<int64_t>();
        if (secs >= 3600) {
            std::cout << "Uptime:   " << secs / 3600 << "h " << (secs % 3600) / 60 << "m\n";
        } else if (secs >= 60) {
            std::cout << "Uptime:   " << secs / 60 << "m " << secs % 60 << "s\n";
        } else {
            std::cout << "Uptime:   " << secs << "s\n";
        }
    }

    if (status.contains("error")) {
        std::cout << "Error:    " << status["error"].get<std::string>() << "\n";
    }

    if (status.contains("nodes") && status["nodes"].is_array()) {
        std::cout << "\nNodes:\n";
        for (const auto& node : status["nodes"]) {
            std::cout << "  " << node.value("name", "?")
                      << " [" << node.value("type", "?") << "]";
            if (node.contains("pid")) {
                std::cout << " pid=" << node["pid"].get<int>();
            }
            if (node.contains("alive")) {
                std::cout << (node["alive"].get<bool>() ? " (alive)" : " (DEAD)");
            }
            std::cout << "\n";

            if (node.contains("config") && !node["config"].is_null()) {
                for (auto& [key, val] : node["config"].items()) {
                    std::cout << "    " << key << "=" << val.dump() << "\n";
                }
            }
        }
    }

    if (status.contains("connections") && status["connections"].is_array()) {
        std::cout << "\nConnections:\n";
        for (const auto& conn : status["connections"]) {
            std::cout << "  " << conn.value("from", "?")
                      << " -> " << conn.value("to", "?") << "\n";
        }
    }

    return 0;
}

static int cmd_metrics(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "Usage: weave metrics <name>\n";
        return 1;
    }

    nlohmann::json params;
    params["name"] = args[0];

    auto result = rpc_call("metrics", params);
    if (!result.has_value()) {
        std::cerr << "Error: " << result.error() << "\n";
        return 1;
    }

    const auto& metrics = result.value();
    if (!metrics.is_array() || metrics.empty()) {
        std::cout << "No metrics available.\n";
        return 0;
    }

    std::cout << std::left
              << std::setw(24) << "NODE"
              << std::setw(12) << "TYPE"
              << std::setw(14) << "THROUGHPUT"
              << std::setw(12) << "LATENCY"
              << std::setw(12) << "MESSAGES"
              << std::setw(12) << "BYTES"
              << "\n";
    std::cout << std::string(86, '-') << "\n";

    for (const auto& m : metrics) {
        char throughput[32];
        std::snprintf(throughput, sizeof(throughput), "%.2f MB/s",
                     m.value("throughput_mbps", 0.0));

        char latency[32];
        std::snprintf(latency, sizeof(latency), "%.1f ms",
                     m.value("latency_ms", 0.0));

        bool is_bottleneck = m.value("bottleneck", false);

        std::cout << std::left
                  << std::setw(24) << m.value("node", "?")
                  << std::setw(12) << m.value("type", "?")
                  << std::setw(14) << throughput
                  << std::setw(12) << latency
                  << std::setw(12) << m.value("messages", static_cast<uint64_t>(0))
                  << std::setw(12) << m.value("bytes", static_cast<uint64_t>(0))
                  << (is_bottleneck ? " [BOTTLENECK]" : "")
                  << "\n";
    }

    return 0;
}

static int cmd_nodes() {
    auto result = rpc_call("nodes");
    if (!result.has_value()) {
        std::cerr << "Error: " << result.error() << "\n";
        return 1;
    }

    const auto& nodes = result.value();
    if (!nodes.is_array() || nodes.empty()) {
        std::cout << "No node types registered.\n";
        return 0;
    }

    std::cout << std::left
              << std::setw(20) << "NODE TYPE"
              << std::setw(16) << "INPUT"
              << std::setw(16) << "OUTPUT"
              << std::setw(8)  << "BUILT-IN"
              << "DESCRIPTION"
              << "\n";
    std::cout << std::string(80, '-') << "\n";

    for (const auto& n : nodes) {
        std::cout << std::left
                  << std::setw(20) << n.value("name", "?")
                  << std::setw(16) << n.value("input", "?")
                  << std::setw(16) << n.value("output", "?")
                  << std::setw(8)  << (n.value("builtin", false) ? "yes" : "no")
                  << n.value("description", "")
                  << "\n";
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

    if (command == "create")  return cmd_create(args);
    if (command == "start")   return cmd_start(args);
    if (command == "stop")    return cmd_stop(args);
    if (command == "delete")  return cmd_delete(args);
    if (command == "list")    return cmd_list();
    if (command == "status")  return cmd_status(args);
    if (command == "metrics") return cmd_metrics(args);
    if (command == "nodes")   return cmd_nodes();

    if (command == "--help" || command == "-h") {
        print_usage();
        return 0;
    }

    std::cerr << "Unknown command: " << command << "\n";
    print_usage();
    return 1;
}
