// tools/splice-cli/main.cpp
// CLI for the straylight-splice zero-copy pipeline stitching daemon.

#include <straylight/ipc.h>
#include <straylight/log.h>

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include <sys/socket.h>
#include <sys/time.h>

static void print_usage() {
    std::cerr
        << "straylight-splice-cli -- Zero-copy pipeline stitching CLI\n\n"
        << "Usage:\n"
        << "  straylight-splice-cli create <pid1> <pid2> [--size=N]  Create splice session\n"
        << "  straylight-splice-cli list                              List active sessions\n"
        << "  straylight-splice-cli stats <session-id>                Show session statistics\n"
        << "  straylight-splice-cli destroy <session-id>              Destroy a session\n";
}

static std::string default_socket() {
    const char* env = std::getenv("SPLICE_SOCKET");
    return env ? env : "/run/straylight/splice.sock";
}

static void print_table_header() {
    std::cout << std::left
              << std::setw(6)  << "ID"
              << std::setw(10) << "PRODUCER"
              << std::setw(10) << "CONSUMER"
              << std::setw(18) << "REGION"
              << std::setw(12) << "SIZE"
              << std::setw(6)  << "ORDER"
              << std::setw(14) << "TRANSFERRED"
              << std::setw(12) << "THROUGHPUT"
              << std::setw(10) << "UPTIME"
              << "\n";
    std::cout << std::string(98, '-') << "\n";
}

static std::string format_bytes(uint64_t bytes) {
    if (bytes < 1024) return std::to_string(bytes) + " B";
    if (bytes < 1048576) return std::to_string(bytes / 1024) + " KiB";
    if (bytes < 1073741824ULL) return std::to_string(bytes / 1048576) + " MiB";
    return std::to_string(bytes / 1073741824ULL) + " GiB";
}

static std::string format_duration(double seconds) {
    if (seconds < 60.0) return std::to_string(static_cast<int>(seconds)) + "s";
    if (seconds < 3600.0) return std::to_string(static_cast<int>(seconds / 60.0)) + "m";
    return std::to_string(static_cast<int>(seconds / 3600.0)) + "h";
}

static void set_read_timeout(straylight::IpcClient& client) {
    timeval tv{};
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(client.fd(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

static straylight::Result<nlohmann::json, std::string>
send_request(straylight::IpcClient& client, const nlohmann::json& request) {
    const std::string payload = request.dump();
    auto sent = client.send(payload);
    if (!sent.has_value()) {
        return straylight::Result<nlohmann::json, std::string>::error(sent.error());
    }

    auto received = client.receive();
    if (!received.has_value()) {
        return straylight::Result<nlohmann::json, std::string>::error(received.error());
    }

    try {
        return straylight::Result<nlohmann::json, std::string>::ok(
            nlohmann::json::parse(received.value()));
    } catch (const nlohmann::json::parse_error& e) {
        return straylight::Result<nlohmann::json, std::string>::error(
            std::string("invalid daemon response: ") + e.what());
    }
}

static int cmd_create(straylight::IpcClient& client, int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "Error: create requires <pid1> <pid2>\n";
        return 1;
    }

    pid_t pid1 = std::atoi(argv[2]);
    pid_t pid2 = std::atoi(argv[3]);
    uint64_t size = 1048576; // Default 1 MiB

    for (int i = 4; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind("--size=", 0) == 0) {
            size = std::stoull(arg.substr(7));
        }
    }

    if (pid1 <= 0 || pid2 <= 0) {
        std::cerr << "Error: invalid PIDs\n";
        return 1;
    }

    nlohmann::json request;
    request["jsonrpc"] = "2.0";
    request["id"] = 1;
    request["method"] = "create";
    request["params"]["producer_pid"] = pid1;
    request["params"]["consumer_pid"] = pid2;
    request["params"]["size"] = size;

    auto response = send_request(client, request);
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
        std::cout << "Splice session created:\n"
                  << "  Session ID:  " << result.value("session_id", uint64_t{0}) << "\n"
                  << "  Producer:    PID " << pid1 << "\n"
                  << "  Consumer:    PID " << pid2 << "\n"
                  << "  Size:        " << format_bytes(size) << "\n"
                  << "  Status:      " << result.value("status", "unknown") << "\n";
    }

    return 0;
}

static int cmd_list(straylight::IpcClient& client) {
    nlohmann::json request;
    request["jsonrpc"] = "2.0";
    request["id"] = 1;
    request["method"] = "list";

    auto response = send_request(client, request);
    if (!response.has_value()) {
        std::cerr << "Error: " << response.error() << "\n";
        return 1;
    }

    const auto& resp = response.value();
    if (resp.contains("error")) {
        std::cerr << "Error: " << resp["error"].dump() << "\n";
        return 1;
    }

    if (!resp.contains("result") || !resp["result"].is_array()) {
        std::cout << "No active splice sessions.\n";
        return 0;
    }

    const auto& sessions = resp["result"];
    if (sessions.empty()) {
        std::cout << "No active splice sessions.\n";
        return 0;
    }

    print_table_header();
    for (const auto& s : sessions) {
        std::cout << std::left
                  << std::setw(6)  << s.value("session_id", uint64_t{0})
                  << std::setw(10) << s.value("producer_pid", 0)
                  << std::setw(10) << s.value("consumer_pid", 0)
                  << std::setw(18) << s.value("region_name", "")
                  << std::setw(12) << format_bytes(s.value("size", uint64_t{0}))
                  << std::setw(6)  << s.value("slab_order", 0)
                  << std::setw(14) << format_bytes(s.value("bytes_transferred", uint64_t{0}));

        double tp = s.value("throughput_mbps", 0.0);
        std::ostringstream tp_str;
        tp_str << std::fixed << std::setprecision(1) << tp << " MB/s";
        std::cout << std::setw(12) << tp_str.str();

        std::cout << std::setw(10) << format_duration(s.value("uptime_seconds", 0.0))
                  << "\n";
    }

    std::cout << "\n" << sessions.size() << " active session(s).\n";
    return 0;
}

static int cmd_stats(straylight::IpcClient& client, int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Error: stats requires <session-id>\n";
        return 1;
    }

    uint64_t session_id = std::stoull(argv[2]);

    nlohmann::json request;
    request["jsonrpc"] = "2.0";
    request["id"] = 1;
    request["method"] = "stats";
    request["params"]["session_id"] = session_id;

    auto response = send_request(client, request);
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
        std::cout << "Splice Session " << r.value("session_id", uint64_t{0}) << " Statistics:\n"
                  << "  Bytes Transferred:  " << format_bytes(r.value("bytes_transferred", uint64_t{0})) << "\n"
                  << "  Push Count:         " << r.value("push_count", uint64_t{0}) << "\n"
                  << "  Pop Count:          " << r.value("pop_count", uint64_t{0}) << "\n";

        double tp = r.value("throughput_mbps", 0.0);
        std::cout << "  Throughput:         " << std::fixed << std::setprecision(2)
                  << tp << " MB/s\n";

        double lat = r.value("avg_latency_us", 0.0);
        std::cout << "  Avg Latency:        " << std::fixed << std::setprecision(1)
                  << lat << " us\n";

        std::cout << "  Ring Available:     " << format_bytes(r.value("ring_available", uint64_t{0})) << "\n"
                  << "  Ring Capacity:      " << format_bytes(r.value("ring_capacity", uint64_t{0})) << "\n";

        double fill = r.value("fill_ratio", 0.0) * 100.0;
        std::cout << "  Fill Ratio:         " << std::fixed << std::setprecision(1)
                  << fill << "%\n"
                  << "  Uptime:             " << format_duration(r.value("uptime_seconds", 0.0)) << "\n";
    }

    return 0;
}

static int cmd_destroy(straylight::IpcClient& client, int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Error: destroy requires <session-id>\n";
        return 1;
    }

    uint64_t session_id = std::stoull(argv[2]);

    nlohmann::json request;
    request["jsonrpc"] = "2.0";
    request["id"] = 1;
    request["method"] = "destroy";
    request["params"]["session_id"] = session_id;

    auto response = send_request(client, request);
    if (!response.has_value()) {
        std::cerr << "Error: " << response.error() << "\n";
        return 1;
    }

    const auto& resp = response.value();
    if (resp.contains("error")) {
        std::cerr << "Error: " << resp["error"].dump() << "\n";
        return 1;
    }

    std::cout << "Splice session " << session_id << " destroyed.\n";
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

    straylight::IpcClient client;
    auto conn = client.connect(default_socket());
    if (!conn.has_value()) {
        std::cerr << "Error: could not connect to splice daemon: " << conn.error() << "\n";
        std::cerr << "Is straylight-splice running?\n";
        return 1;
    }
    set_read_timeout(client);

    if (command == "create") {
        return cmd_create(client, argc, argv);
    } else if (command == "list") {
        return cmd_list(client);
    } else if (command == "stats") {
        return cmd_stats(client, argc, argv);
    } else if (command == "destroy") {
        return cmd_destroy(client, argc, argv);
    } else {
        std::cerr << "Error: unknown command '" << command << "'\n";
        print_usage();
        return 1;
    }
}
