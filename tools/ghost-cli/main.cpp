// tools/ghost-cli/main.cpp
// CLI for the straylight-ghost transparent process migration daemon.

#include <straylight/ipc_client.h>
#include <straylight/log.h>

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>

static void print_usage() {
    std::cerr
        << "straylight-ghost -- Transparent process migration CLI\n\n"
        << "Usage:\n"
        << "  straylight-ghost migrate <pid|name> <target-host>     Full migration\n"
        << "  straylight-ghost lazy-migrate <pid> <target-host>     Lazy migration\n"
        << "  straylight-ghost status [migration-id]                Show status\n"
        << "  straylight-ghost list                                 List all migrations\n"
        << "  straylight-ghost cancel <migration-id>                Cancel migration\n";
}

static std::string default_socket() {
    const char* env = std::getenv("GHOST_SOCKET");
    return env ? env : "/run/straylight/ghost.sock";
}

static const char* BOLD = "\033[1m";
static const char* RESET = "\033[0m";
static const char* GREEN = "\033[32m";
static const char* YELLOW = "\033[33m";
static const char* RED = "\033[31m";
static const char* CYAN = "\033[36m";

static const char* state_color(const std::string& state) {
    if (state == "complete") return GREEN;
    if (state == "failed" || state == "cancelled") return RED;
    if (state == "streaming" || state == "lazy-active") return CYAN;
    return YELLOW;
}

static std::string format_pages(uint64_t pages) {
    double mb = static_cast<double>(pages * 4096) / (1024.0 * 1024.0);
    std::ostringstream oss;
    oss << pages << " (" << std::fixed << std::setprecision(1) << mb << " MiB)";
    return oss.str();
}

static std::string format_time(double seconds) {
    if (seconds < 1.0) {
        int ms = static_cast<int>(seconds * 1000);
        return std::to_string(ms) + "ms";
    }
    if (seconds < 60.0) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1) << seconds << "s";
        return oss.str();
    }
    int min = static_cast<int>(seconds / 60.0);
    int sec = static_cast<int>(seconds) % 60;
    return std::to_string(min) + "m" + std::to_string(sec) + "s";
}

static void print_progress_bar(uint64_t done, uint64_t total, int width = 30) {
    double ratio = (total > 0) ? static_cast<double>(done) / static_cast<double>(total) : 0;
    int filled = static_cast<int>(ratio * width);

    std::cout << "[";
    for (int i = 0; i < width; ++i) {
        if (i < filled) std::cout << "#";
        else std::cout << ".";
    }
    std::cout << "] " << std::fixed << std::setprecision(1) << (ratio * 100.0) << "%";
}

static int cmd_migrate(straylight::IpcJsonClient& client, int argc, char* argv[],
                        bool lazy) {
    if (argc < 4) {
        std::cerr << "Error: " << (lazy ? "lazy-migrate" : "migrate")
                  << " requires <pid|name> <target-host>\n";
        return 1;
    }

    std::string pid_or_name = argv[2];
    std::string target = argv[3];

    nlohmann::json request;
    request["jsonrpc"] = "2.0";
    request["id"] = 1;
    request["method"] = lazy ? "lazy_migrate" : "migrate";

    // Try to parse as PID, fall back to name
    try {
        int pid = std::stoi(pid_or_name);
        request["params"]["pid"] = pid;
    } catch (...) {
        request["params"]["name"] = pid_or_name;
    }

    request["params"]["target_host"] = target;

    // Parse additional options
    for (int i = 4; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind("--port=", 0) == 0) {
            request["params"]["port"] = std::stoi(arg.substr(7));
        } else if (arg == "--no-compress") {
            request["params"]["compress_level"] = 0;
        } else if (arg == "--transfer-network") {
            request["params"]["transfer_network"] = true;
        }
    }

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
        const auto& r = resp["result"];
        std::cout << GREEN << "Migration started:" << RESET << "\n"
                  << "  Migration ID:  " << r.value("migration_id", uint64_t{0}) << "\n"
                  << "  Source PID:    " << r.value("pid", 0) << "\n"
                  << "  Target:        " << r.value("target", "") << "\n"
                  << "  Mode:          " << r.value("mode", "") << "\n"
                  << "  Status:        " << r.value("status", "") << "\n"
                  << "\nUse 'straylight-ghost status " << r.value("migration_id", uint64_t{0})
                  << "' to track progress.\n";
    }
    return 0;
}

static int cmd_status(straylight::IpcJsonClient& client, int argc, char* argv[]) {
    nlohmann::json request;
    request["jsonrpc"] = "2.0";
    request["id"] = 1;
    request["method"] = "status";

    if (argc >= 3) {
        request["params"]["migration_id"] = std::stoull(argv[2]);
    }

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

    if (!resp.contains("result")) return 0;

    const auto& r = resp["result"];

    if (r.is_array()) {
        // List of active migrations
        if (r.empty()) {
            std::cout << "No active migrations.\n";
            return 0;
        }

        for (const auto& m : r) {
            std::string state = m.value("state", "unknown");
            std::cout << state_color(state) << BOLD << "Migration "
                      << m.value("migration_id", uint64_t{0}) << RESET << "\n"
                      << "  PID " << m.value("pid", 0)
                      << " -> " << m.value("target", "") << "\n"
                      << "  State: " << state_color(state) << state << RESET << "\n"
                      << "  Pages: " << m.value("pages_transferred", uint64_t{0})
                      << "/" << (m.value("pages_transferred", uint64_t{0}) +
                                  m.value("pages_remaining", uint64_t{0})) << "  ";
            print_progress_bar(m.value("pages_transferred", uint64_t{0}),
                               m.value("pages_transferred", uint64_t{0}) +
                               m.value("pages_remaining", uint64_t{0}));
            std::cout << "\n"
                      << "  Bandwidth: " << std::fixed << std::setprecision(1)
                      << m.value("bandwidth_mbps", 0.0) << " MB/s"
                      << "  ETA: " << format_time(m.value("eta_seconds", 0.0)) << "\n\n";
        }
    } else {
        // Single migration detail
        std::string state = r.value("state", "unknown");
        uint64_t total = r.value("total_pages", uint64_t{0});
        uint64_t transferred = r.value("pages_transferred", uint64_t{0});

        std::cout << BOLD << "Migration " << r.value("migration_id", uint64_t{0})
                  << RESET << "\n\n"
                  << "  Source PID:        " << r.value("pid", 0) << "\n"
                  << "  Target:            " << r.value("target", "") << "\n"
                  << "  State:             " << state_color(state) << BOLD
                  << state << RESET << "\n"
                  << "  Total Pages:       " << format_pages(total) << "\n"
                  << "  Transferred:       " << format_pages(transferred) << "\n"
                  << "  Remaining:         " << format_pages(r.value("pages_remaining", uint64_t{0})) << "\n"
                  << "  Hot Pages:         " << r.value("hot_pages", uint64_t{0}) << "\n"
                  << "  Bandwidth:         " << std::fixed << std::setprecision(1)
                  << r.value("bandwidth_mbps", 0.0) << " MB/s\n"
                  << "  Elapsed:           " << format_time(r.value("elapsed_seconds", 0.0)) << "\n"
                  << "  ETA:               " << format_time(r.value("eta_seconds", 0.0)) << "\n";

        std::cout << "\n  Progress: ";
        print_progress_bar(transferred, total, 40);
        std::cout << "\n";

        if (r.contains("error") && !r["error"].get<std::string>().empty()) {
            std::cout << "\n  " << RED << "Error: " << r["error"].get<std::string>()
                      << RESET << "\n";
        }
    }

    return 0;
}

static int cmd_list(straylight::IpcJsonClient& client) {
    nlohmann::json request;
    request["jsonrpc"] = "2.0";
    request["id"] = 1;
    request["method"] = "list";

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

    if (!resp.contains("result") || !resp["result"].is_array() || resp["result"].empty()) {
        std::cout << "No migrations recorded.\n";
        return 0;
    }

    std::cout << std::left
              << std::setw(6)  << "ID"
              << std::setw(8)  << "PID"
              << std::setw(20) << "TARGET"
              << std::setw(14) << "STATE"
              << std::setw(12) << "TRANSFERRED"
              << std::setw(10) << "TOTAL"
              << "\n"
              << std::string(70, '-') << "\n";

    for (const auto& m : resp["result"]) {
        std::string state = m.value("state", "unknown");
        std::cout << std::left
                  << std::setw(6)  << m.value("migration_id", uint64_t{0})
                  << std::setw(8)  << m.value("pid", 0)
                  << std::setw(20) << m.value("target", "")
                  << state_color(state) << std::setw(14) << state << RESET
                  << std::setw(12) << m.value("pages_transferred", uint64_t{0})
                  << std::setw(10) << m.value("total_pages", uint64_t{0})
                  << "\n";
    }

    std::cout << "\n" << resp["result"].size() << " migration(s) total.\n";
    return 0;
}

static int cmd_cancel(straylight::IpcJsonClient& client, int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Error: cancel requires <migration-id>\n";
        return 1;
    }

    nlohmann::json request;
    request["jsonrpc"] = "2.0";
    request["id"] = 1;
    request["method"] = "cancel";
    request["params"]["migration_id"] = std::stoull(argv[2]);

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

    std::cout << "Migration " << argv[2] << " cancelled. Source process resumed.\n";
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
        std::cerr << "Error: could not connect to ghost daemon: " << conn.error() << "\n";
        std::cerr << "Is straylight-ghost running?\n";
        return 1;
    }

    if (command == "migrate")           return cmd_migrate(client, argc, argv, false);
    else if (command == "lazy-migrate") return cmd_migrate(client, argc, argv, true);
    else if (command == "status")       return cmd_status(client, argc, argv);
    else if (command == "list")         return cmd_list(client);
    else if (command == "cancel")       return cmd_cancel(client, argc, argv);
    else {
        std::cerr << "Error: unknown command '" << command << "'\n";
        print_usage();
        return 1;
    }
}
