// tools/hooks-cli/main.cpp
// CLI front-end for straylight-hooks daemon.
#include <straylight/ipc.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include <sys/socket.h>
#include <sys/time.h>

static const char* SOCKET_PATH = "/run/straylight/hooks.sock";

static void set_read_timeout(straylight::IpcClient& client) {
    timeval tv{};
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(client.fd(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

static void print_usage() {
    std::cerr
        << "straylight-hooks-cli — system event hook manager CLI\n"
        << "\n"
        << "Usage:\n"
        << "  straylight-hooks-cli list\n"
        << "  straylight-hooks-cli add <event> <script> [--priority <n>] [--timeout <s>]\n"
        << "  straylight-hooks-cli remove <id>\n"
        << "  straylight-hooks-cli test <event>\n"
        << "  straylight-hooks-cli history\n"
        << "\n"
        << "Events: boot, shutdown, suspend, resume, network-up, network-down,\n"
        << "        usb-attach, usb-detach, lid-open, lid-close, battery-low,\n"
        << "        power-ac, power-battery\n";
}

static nlohmann::json send_command(const std::string& cmd,
                                    const nlohmann::json& params = {}) {
    straylight::IpcClient client;
    auto conn = client.connect(SOCKET_PATH);
    if (!conn.has_value()) {
        std::cerr << "Error: cannot connect to hooks daemon: " << conn.error() << "\n";
        std::exit(1);
    }
    set_read_timeout(client);

    nlohmann::json request;
    request["cmd"] = cmd;
    if (!params.empty()) {
        request["params"] = params;
    }

    const std::string payload = request.dump();
    auto sent = client.send(payload);
    if (!sent.has_value()) {
        std::cerr << "Error: " << sent.error() << "\n";
        std::exit(1);
    }

    auto received = client.receive();
    if (!received.has_value()) {
        std::cerr << "Error: " << received.error() << "\n";
        std::exit(1);
    }

    try {
        return nlohmann::json::parse(received.value());
    } catch (const nlohmann::json::parse_error& e) {
        std::cerr << "Error: invalid daemon response: " << e.what() << "\n";
        std::exit(1);
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

    if (command == "list") {
        auto resp = send_command("list");
        if (resp.value("status", "") != "ok") {
            std::cerr << "Error: " << resp.value("message", "unknown") << "\n";
            return 1;
        }
        auto& hooks = resp["hooks"];
        if (hooks.empty()) {
            std::cout << "No hooks registered.\n";
            return 0;
        }
        printf("%-15s %-15s %-35s %-8s %-8s %-8s\n",
               "ID", "EVENT", "SCRIPT", "ENABLED", "PRI", "TIMEOUT");
        for (const auto& h : hooks) {
            printf("%-15s %-15s %-35s %-8s %-8d %-8ds\n",
                   h.value("id", "").c_str(),
                   h.value("event", "").c_str(),
                   h.value("script", "").c_str(),
                   h.value("enabled", true) ? "yes" : "no",
                   h.value("priority", 50),
                   h.value("timeout", 30));
        }
        return 0;
    }

    if (command == "add") {
        if (argc < 4) {
            std::cerr << "Error: 'add' requires an event and script path\n";
            return 1;
        }
        nlohmann::json params;
        params["event"] = argv[2];
        params["script"] = argv[3];
        for (int i = 4; i < argc; ++i) {
            if (std::strcmp(argv[i], "--priority") == 0 && i + 1 < argc) {
                params["priority"] = std::stoi(argv[++i]);
            } else if (std::strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
                params["timeout_seconds"] = std::stoi(argv[++i]);
            }
        }
        auto resp = send_command("add", params);
        if (resp.value("status", "") == "ok") {
            std::cout << "Hook added: " << resp.value("id", "") << "\n";
        } else {
            std::cerr << "Error: " << resp.value("message", "unknown") << "\n";
            return 1;
        }
        return 0;
    }

    if (command == "remove") {
        if (argc < 3) {
            std::cerr << "Error: 'remove' requires a hook ID\n";
            return 1;
        }
        nlohmann::json params;
        params["id"] = argv[2];
        auto resp = send_command("remove", params);
        if (resp.value("status", "") == "ok") {
            std::cout << "Hook removed.\n";
        } else {
            std::cerr << "Error: " << resp.value("message", "unknown") << "\n";
            return 1;
        }
        return 0;
    }

    if (command == "test") {
        if (argc < 3) {
            std::cerr << "Error: 'test' requires an event name\n";
            return 1;
        }
        nlohmann::json params;
        params["event"] = argv[2];
        std::cout << "Firing test event '" << argv[2] << "'...\n";
        auto resp = send_command("test", params);
        if (resp.value("status", "") != "ok") {
            std::cerr << "Error: " << resp.value("message", "unknown") << "\n";
            return 1;
        }
        auto& results = resp["results"];
        if (results.empty()) {
            std::cout << "No hooks registered for event '" << argv[2] << "'.\n";
        } else {
            for (const auto& r : results) {
                std::string status = r.value("exit_code", 0) == 0 ?
                    "\033[32mOK\033[0m" : "\033[31mFAILED\033[0m";
                if (r.value("timed_out", false)) status = "\033[31mTIMEOUT\033[0m";
                std::cout << "  " << r.value("hook_id", "") << " — " << status
                          << " (" << r.value("duration_ms", 0) << "ms)\n";
            }
        }
        return 0;
    }

    if (command == "history") {
        auto resp = send_command("history");
        if (resp.value("status", "") != "ok") {
            std::cerr << "Error: " << resp.value("message", "unknown") << "\n";
            return 1;
        }
        auto& history = resp["history"];
        if (history.empty()) {
            std::cout << "No hook execution history.\n";
            return 0;
        }
        printf("%-20s %-15s %-15s %-8s %-10s\n",
               "TIMESTAMP", "HOOK", "EVENT", "EXIT", "DURATION");
        for (const auto& h : history) {
            std::string exit_str = h.value("timed_out", false) ? "TIMEOUT" :
                                   std::to_string(h.value("exit_code", -1));
            printf("%-20s %-15s %-15s %-8s %-10dms\n",
                   h.value("timestamp", "").c_str(),
                   h.value("hook_id", "").c_str(),
                   h.value("event", "").c_str(),
                   exit_str.c_str(),
                   h.value("duration_ms", 0));
        }
        return 0;
    }

    std::cerr << "Unknown command: " << command << "\n";
    print_usage();
    return 1;
}
