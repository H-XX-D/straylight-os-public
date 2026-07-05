// tools/watchdog-cli/main.cpp
// CLI front-end for straylight-watchdog daemon.
#include <straylight/ipc_client.h>

#include <cstring>
#include <iostream>
#include <string>

static const char* SOCKET_PATH = "/run/straylight/watchdog.sock";

static void print_usage() {
    std::cerr
        << "straylight-watchdog — process watchdog CLI\n"
        << "\n"
        << "Usage:\n"
        << "  straylight-watchdog watch <service> [--unit <unit>] [--retries <n>]\n"
        << "  straylight-watchdog unwatch <service>\n"
        << "  straylight-watchdog list\n"
        << "  straylight-watchdog status <service>\n"
        << "  straylight-watchdog history <service>\n";
}

static nlohmann::json send_command(const std::string& cmd,
                                    const nlohmann::json& params = {}) {
    straylight::IpcJsonClient client;
    auto conn = client.connect(SOCKET_PATH);
    if (!conn.has_value()) {
        std::cerr << "Error: cannot connect to watchdog daemon: " << conn.error() << "\n";
        std::exit(1);
    }
    auto res = client.command(cmd, params);
    if (!res.has_value()) {
        std::cerr << "Error: " << res.error() << "\n";
        std::exit(1);
    }
    return res.value();
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
    // watch <service>
    // -----------------------------------------------------------------------
    if (command == "watch") {
        if (argc < 3) {
            std::cerr << "Error: 'watch' requires a service name\n";
            return 1;
        }
        nlohmann::json params;
        params["name"] = argv[2];

        // Parse optional flags
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "--unit") == 0 && i + 1 < argc) {
                params["unit"] = argv[++i];
            } else if (std::strcmp(argv[i], "--retries") == 0 && i + 1 < argc) {
                params["max_retries"] = std::stoi(argv[++i]);
            }
        }

        auto resp = send_command("watch", params);
        if (resp.value("status", "") == "ok") {
            std::cout << "Now watching: " << argv[2] << "\n";
        } else {
            std::cerr << "Error: " << resp.value("message", "unknown error") << "\n";
            return 1;
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // unwatch <service>
    // -----------------------------------------------------------------------
    if (command == "unwatch") {
        if (argc < 3) {
            std::cerr << "Error: 'unwatch' requires a service name\n";
            return 1;
        }
        nlohmann::json params;
        params["name"] = argv[2];
        auto resp = send_command("unwatch", params);
        if (resp.value("status", "") == "ok") {
            std::cout << "Stopped watching: " << argv[2] << "\n";
        } else {
            std::cerr << "Error: " << resp.value("message", "unknown error") << "\n";
            return 1;
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // list
    // -----------------------------------------------------------------------
    if (command == "list") {
        auto resp = send_command("list");
        if (resp.value("status", "") != "ok") {
            std::cerr << "Error: " << resp.value("message", "unknown") << "\n";
            return 1;
        }

        auto& services = resp["services"];
        if (services.empty()) {
            std::cout << "No services being watched.\n";
            return 0;
        }

        std::cout << "Watched services:\n";
        std::cout << "  " << std::left;
        printf("%-30s %-8s %-8s %-10s %-8s\n", "NAME", "PID", "STATUS", "FAILURES", "RESTARTS");
        for (const auto& svc : services) {
            std::string status = svc.value("running", false) ? "\033[32mUP\033[0m" : "\033[31mDOWN\033[0m";
            printf("  %-30s %-8d %-18s %-10d %-8d\n",
                   svc.value("name", "").c_str(),
                   svc.value("pid", 0),
                   status.c_str(),
                   svc.value("failures", 0),
                   svc.value("restarts", 0));
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // status <service>
    // -----------------------------------------------------------------------
    if (command == "status") {
        if (argc < 3) {
            std::cerr << "Error: 'status' requires a service name\n";
            return 1;
        }
        nlohmann::json params;
        params["name"] = argv[2];
        auto resp = send_command("status", params);
        if (resp.value("status", "") != "ok") {
            std::cerr << "Error: " << resp.value("message", "unknown") << "\n";
            return 1;
        }

        std::string run_status = resp.value("running", false) ? "\033[32mUP\033[0m" : "\033[31mDOWN\033[0m";
        std::cout << "Service:    " << resp.value("name", "") << "\n"
                  << "PID:        " << resp.value("pid", 0) << "\n"
                  << "Status:     " << run_status << "\n"
                  << "Failures:   " << resp.value("failures", 0) << "\n"
                  << "Restarts:   " << resp.value("restarts", 0) << "\n"
                  << "Last error: " << resp.value("last_error", "none") << "\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // history <service>
    // -----------------------------------------------------------------------
    if (command == "history") {
        if (argc < 3) {
            std::cerr << "Error: 'history' requires a service name\n";
            return 1;
        }
        nlohmann::json params;
        params["name"] = argv[2];
        auto resp = send_command("history", params);
        if (resp.value("status", "") != "ok") {
            std::cerr << "Error: " << resp.value("message", "unknown") << "\n";
            return 1;
        }

        auto& history = resp["history"];
        if (history.empty()) {
            std::cout << "No history for " << argv[2] << ".\n";
            return 0;
        }

        std::cout << "History for " << argv[2] << ":\n";
        for (const auto& entry : history) {
            std::cout << "  " << entry.get<std::string>() << "\n";
        }
        return 0;
    }

    std::cerr << "Unknown command: " << command << "\n";
    print_usage();
    return 1;
}
