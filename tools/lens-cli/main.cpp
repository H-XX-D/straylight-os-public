// tools/lens-cli/main.cpp
// CLI for the straylight-lens full-stack request tracing daemon.

#include <straylight/ipc.h>
#include <straylight/log.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include <sys/socket.h>
#include <sys/time.h>

static void print_usage() {
    std::cerr
        << "straylight-lens -- Full-stack request tracing CLI\n\n"
        << "Usage:\n"
        << "  straylight-lens start [--id=X]                 Start trace collection\n"
        << "  straylight-lens stop                            Stop and save trace\n"
        << "  straylight-lens show <trace-id>                 Show trace timeline\n"
        << "  straylight-lens list                            List stored traces\n"
        << "  straylight-lens export <trace-id> [--format=F]  Export (chrome|json)\n"
        << "  straylight-lens bottleneck <trace-id>           Show bottleneck analysis\n"
        << "  straylight-lens live                            Show live collection status\n";
}

static std::string default_socket() {
    const char* env = std::getenv("LENS_SOCKET");
    return env ? env : "/run/straylight/lens.sock";
}

static std::string format_ns(uint64_t ns) {
    if (ns < 1000) return std::to_string(ns) + "ns";
    if (ns < 1000000) return std::to_string(ns / 1000) + "us";
    if (ns < 1000000000ULL) {
        double ms = static_cast<double>(ns) / 1e6;
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << ms << "ms";
        return oss.str();
    }
    double s = static_cast<double>(ns) / 1e9;
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3) << s << "s";
    return oss.str();
}

// ANSI color codes for layer display
static const char* layer_color(const std::string& layer) {
    if (layer == "compositor") return "\033[36m";   // cyan
    if (layer == "ipc")        return "\033[33m";   // yellow
    if (layer == "vpu")        return "\033[35m";   // magenta
    if (layer == "gpu")        return "\033[32m";   // green
    if (layer == "app")        return "\033[34m";   // blue
    if (layer == "kernel")     return "\033[31m";   // red
    if (layer == "network")    return "\033[37m";   // white
    return "\033[0m";
}

static const char* RESET = "\033[0m";
static const char* BOLD = "\033[1m";
static const char* DIM = "\033[2m";

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

static int cmd_start(straylight::IpcClient& client, int argc, char* argv[]) {
    std::string corr_id;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind("--id=", 0) == 0) {
            corr_id = arg.substr(5);
        }
    }

    nlohmann::json request;
    request["jsonrpc"] = "2.0";
    request["id"] = 1;
    request["method"] = "start";
    if (!corr_id.empty()) {
        request["params"]["correlation_id"] = corr_id;
    }

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
        std::cout << "Trace collection started:\n"
                  << "  Trace ID:       " << r.value("trace_id", "") << "\n"
                  << "  Correlation ID: " << r.value("correlation_id", "") << "\n"
                  << "  Status:         " << r.value("status", "") << "\n"
                  << "\nUse 'straylight-lens stop' to end collection.\n";
    }
    return 0;
}

static int cmd_stop(straylight::IpcClient& client) {
    nlohmann::json request;
    request["jsonrpc"] = "2.0";
    request["id"] = 1;
    request["method"] = "stop";

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
        std::cout << "Trace collection stopped:\n"
                  << "  Trace ID:          " << r.value("trace_id", "") << "\n"
                  << "  Events:            " << r.value("event_count", 0) << "\n"
                  << "  Duration:          " << format_ns(r.value("duration_ns", uint64_t{0})) << "\n"
                  << "  Critical Path:     " << r.value("critical_path_events", 0) << " events, "
                  << format_ns(r.value("critical_path_duration_ns", uint64_t{0})) << "\n"
                  << "  Status:            " << r.value("status", "") << "\n";
    }
    return 0;
}

static int cmd_show(straylight::IpcClient& client, int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Error: show requires <trace-id>\n";
        return 1;
    }

    nlohmann::json request;
    request["jsonrpc"] = "2.0";
    request["id"] = 1;
    request["method"] = "show";
    request["params"]["trace_id"] = argv[2];

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

    if (!resp.contains("result")) return 0;
    const auto& r = resp["result"];

    std::cout << BOLD << "Trace: " << r.value("trace_id", "") << RESET << "\n"
              << "  Events: " << r.value("event_count", 0)
              << "  Duration: " << format_ns(r.value("duration_ns", uint64_t{0})) << "\n\n";

    // Print timeline by layer
    if (r.contains("timeline") && r["timeline"].is_object()) {
        std::cout << BOLD << "Timeline by Layer:" << RESET << "\n";

        // Layer order
        const char* layers[] = {"compositor", "ipc", "app", "vpu", "gpu", "kernel", "network"};

        for (const char* layer : layers) {
            if (!r["timeline"].contains(layer)) continue;
            const auto& events = r["timeline"][layer];
            if (events.empty()) continue;

            std::cout << "\n  " << layer_color(layer) << BOLD
                      << std::setw(12) << std::left << layer << RESET << " ";

            // Print event blocks
            for (const auto& ev : events) {
                std::string type = ev.value("event_type", "?");
                uint64_t dur = ev.value("duration_ns", uint64_t{0});

                std::cout << layer_color(layer);
                if (dur > 0) {
                    // Duration block: width proportional to duration (min 3 chars)
                    int width = std::max(3, static_cast<int>(dur / 100000)); // 0.1ms per char
                    width = std::min(width, 40);
                    std::cout << "[";
                    std::string label = type + " " + format_ns(dur);
                    if (static_cast<int>(label.size()) > width - 2) {
                        label = label.substr(0, static_cast<size_t>(width - 2));
                    }
                    std::cout << label;
                    int padding = width - 2 - static_cast<int>(label.size());
                    for (int p = 0; p < padding; ++p) std::cout << " ";
                    std::cout << "]";
                } else {
                    std::cout << "|" << type;
                }
                std::cout << RESET << " ";
            }
            std::cout << "\n";
        }
    }

    // Print critical path
    if (r.contains("critical_path")) {
        const auto& cp = r["critical_path"];
        std::cout << "\n" << BOLD << "Critical Path:" << RESET
                  << " (" << format_ns(cp.value("total_duration_ns", uint64_t{0}))
                  << " total, " << format_ns(cp.value("total_work_ns", uint64_t{0}))
                  << " work, " << format_ns(cp.value("total_gap_ns", uint64_t{0}))
                  << " gaps)\n";

        if (cp.contains("events") && cp["events"].is_array()) {
            for (size_t i = 0; i < cp["events"].size(); ++i) {
                const auto& ev = cp["events"][i];
                std::string layer = ev.value("layer", "?");
                std::cout << "  " << (i + 1) << ". "
                          << layer_color(layer) << layer << RESET
                          << "/" << ev.value("event_type", "?")
                          << " [" << format_ns(ev.value("duration_ns", uint64_t{0})) << "]";
                if (i + 1 < cp["events"].size()) {
                    std::cout << " -> ";
                }
                std::cout << "\n";
            }
        }
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

    if (!resp.contains("result") || !resp["result"].is_array() || resp["result"].empty()) {
        std::cout << "No stored traces.\n";
        return 0;
    }

    std::cout << std::left
              << std::setw(32) << "TRACE ID"
              << std::setw(24) << "CORRELATION ID"
              << std::setw(8)  << "EVENTS"
              << std::setw(12) << "DURATION"
              << std::setw(10) << "SIZE"
              << "\n"
              << std::string(86, '-') << "\n";

    for (const auto& t : resp["result"]) {
        std::cout << std::left
                  << std::setw(32) << t.value("trace_id", "")
                  << std::setw(24) << t.value("correlation_id", "")
                  << std::setw(8)  << t.value("event_count", 0)
                  << std::setw(12) << format_ns(t.value("duration_ns", uint64_t{0}));

        uint64_t sz = t.value("file_size", uint64_t{0});
        if (sz < 1024) std::cout << std::setw(10) << (std::to_string(sz) + " B");
        else std::cout << std::setw(10) << (std::to_string(sz / 1024) + " KiB");
        std::cout << "\n";
    }

    std::cout << "\n" << resp["result"].size() << " trace(s) stored.\n";
    return 0;
}

static int cmd_export(straylight::IpcClient& client, int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Error: export requires <trace-id>\n";
        return 1;
    }

    std::string trace_id = argv[2];
    std::string format = "chrome";
    std::string output;

    for (int i = 3; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind("--format=", 0) == 0) {
            format = arg.substr(9);
        } else if ((arg == "--output" || arg == "-o") && i + 1 < argc) {
            output = argv[++i];
        }
    }

    nlohmann::json request;
    request["jsonrpc"] = "2.0";
    request["id"] = 1;
    request["method"] = "export";
    request["params"]["trace_id"] = trace_id;
    request["params"]["format"] = format;

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

    if (resp.contains("result") && resp["result"].contains("data")) {
        const std::string& data = resp["result"]["data"].get_ref<const std::string&>();
        if (output.empty()) {
            std::cout << data << "\n";
        } else {
            std::ofstream out(output, std::ios::trunc);
            if (!out) {
                std::cerr << "Error: cannot write to " << output << "\n";
                return 1;
            }
            out << data;
            std::cout << "Exported " << format << " trace to " << output << "\n";
        }
    }

    return 0;
}

static int cmd_bottleneck(straylight::IpcClient& client, int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Error: bottleneck requires <trace-id>\n";
        return 1;
    }

    nlohmann::json request;
    request["jsonrpc"] = "2.0";
    request["id"] = 1;
    request["method"] = "bottleneck";
    request["params"]["trace_id"] = argv[2];

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
        std::string layer = r.value("layer", "unknown");

        std::cout << BOLD << "Bottleneck Analysis:" << RESET << "\n\n"
                  << "  Layer:      " << layer_color(layer) << BOLD << layer << RESET << "\n"
                  << "  Event:      " << r.value("event_type", "unknown") << "\n"
                  << "  Duration:   " << format_ns(r.value("duration_ns", uint64_t{0})) << "\n";

        double frac = r.value("fraction", 0.0) * 100.0;
        std::cout << "  Impact:     " << std::fixed << std::setprecision(1)
                  << frac << "% of critical path\n"
                  << "  Process:    PID " << r.value("pid", 0) << "\n\n"
                  << BOLD << "  Suggestion: " << RESET << r.value("suggestion", "") << "\n";
    }
    return 0;
}

static int cmd_live(straylight::IpcClient& client) {
    nlohmann::json request;
    request["jsonrpc"] = "2.0";
    request["id"] = 1;
    request["method"] = "live";

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
        std::string status = r.value("status", "unknown");

        if (status == "idle") {
            std::cout << "No trace in progress.\n";
        } else {
            std::cout << "Live Trace Status:\n"
                      << "  Trace ID:    " << r.value("trace_id", "") << "\n"
                      << "  Status:      " << status << "\n"
                      << "  Events:      " << r.value("event_count", 0) << "\n"
                      << "  Duration:    " << format_ns(r.value("duration_ns", uint64_t{0})) << "\n";

            if (r.contains("layers") && r["layers"].is_object()) {
                std::cout << "\n  Events by Layer:\n";
                for (auto& [layer, count] : r["layers"].items()) {
                    std::cout << "    " << layer_color(layer) << std::setw(12) << std::left
                              << layer << RESET << " " << count << "\n";
                }
            }
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

    straylight::IpcClient client;
    auto conn = client.connect(default_socket());
    if (!conn.has_value()) {
        std::cerr << "Error: could not connect to lens daemon: " << conn.error() << "\n";
        std::cerr << "Is straylight-lens running?\n";
        return 1;
    }
    set_read_timeout(client);

    if (command == "start")          return cmd_start(client, argc, argv);
    else if (command == "stop")      return cmd_stop(client);
    else if (command == "show")      return cmd_show(client, argc, argv);
    else if (command == "list")      return cmd_list(client);
    else if (command == "export")    return cmd_export(client, argc, argv);
    else if (command == "bottleneck") return cmd_bottleneck(client, argc, argv);
    else if (command == "live")      return cmd_live(client);
    else {
        std::cerr << "Error: unknown command '" << command << "'\n";
        print_usage();
        return 1;
    }
}
