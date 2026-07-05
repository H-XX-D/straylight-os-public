// tools/notify-cli/main.cpp
// straylight-notify-cli — CLI client for the notification daemon.
#include <straylight/ipc.h>
#include <straylight/log.h>

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>

static constexpr const char* kSocketPath = "/run/straylight/notify.sock";

static void print_usage() {
    std::cerr
        << "straylight-notify-cli — Notification management\n\n"
        << "Usage:\n"
        << "  straylight-notify-cli send <summary> [--body=X] [--urgency=low|normal|critical]\n"
        << "  straylight-notify-cli history [--app=X]\n"
        << "  straylight-notify-cli clear\n"
        << "  straylight-notify-cli dnd [on|off|schedule]\n"
        << "  straylight-notify-cli rules [add|list|remove]\n"
        << "\nExamples:\n"
        << "  straylight-notify-cli send 'Build Complete' --body='Project compiled successfully'\n"
        << "  straylight-notify-cli send 'Low Battery' --urgency=critical\n"
        << "  straylight-notify-cli history --app=firefox\n"
        << "  straylight-notify-cli dnd on\n"
        << "  straylight-notify-cli dnd schedule 22 0 7 0\n"
        << "  straylight-notify-cli rules add 'slack' suppress\n";
}

/// Extract --key=value from argv.
static std::string extract_flag(int argc, char* argv[], const std::string& prefix) {
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.substr(0, prefix.size()) == prefix) {
            return arg.substr(prefix.size());
        }
    }
    return "";
}

/// Send a message to the notify daemon and get a response.
static int send_command(const std::string& message) {
    straylight::IpcClient client;
    auto conn_result = client.connect(kSocketPath);
    if (!conn_result.has_value()) {
        std::cerr << "Error: cannot connect to notification daemon at " << kSocketPath << "\n";
        std::cerr << "Is straylight-notify running?\n";
        return 1;
    }

    timeval timeout{};
    timeout.tv_sec = 3;
    setsockopt(client.fd(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(client.fd(), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    auto send_result = client.send(message);
    if (!send_result.has_value()) {
        std::cerr << "Error: send failed: " << send_result.error() << "\n";
        return 1;
    }

    auto recv_result = client.receive();
    if (!recv_result.has_value()) {
        std::cerr << "Error: receive failed: " << recv_result.error() << "\n";
        return 1;
    }

    const std::string& response = recv_result.value();

    // Check for error responses.
    if (response.substr(0, 3) == "ERR") {
        std::cerr << "Error: " << response.substr(4) << "\n";
        return 1;
    }

    // Print response.
    std::cout << response;
    if (!response.empty() && response.back() != '\n') {
        std::cout << "\n";
    }

    return 0;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string cmd = argv[1];

    if (cmd == "--help" || cmd == "-h") {
        print_usage();
        return 0;
    }

    if (cmd == "send") {
        if (argc < 3) {
            std::cerr << "Usage: straylight-notify-cli send <summary> [--body=X] [--urgency=X]\n";
            return 1;
        }

        std::string summary = argv[2];
        std::string body = extract_flag(argc, argv, "--body=");
        std::string urgency = extract_flag(argc, argv, "--urgency=");
        if (urgency.empty()) urgency = "normal";

        // Protocol: SEND <urgency> <app> <summary>\t<body>
        std::string app = "cli";
        std::string message = "SEND " + urgency + " " + app + " " + summary + "\t" + body;

        int rc = send_command(message);
        if (rc == 0) {
            std::cout << "Notification sent.\n";
        }
        return rc;

    } else if (cmd == "history") {
        std::string app_filter = extract_flag(argc, argv, "--app=");
        std::string message = "HISTORY";
        if (!app_filter.empty()) message += " " + app_filter;

        int rc = send_command(message);
        if (rc != 0) return rc;

        // The response is already printed by send_command.
        // Format it more nicely.
        return 0;

    } else if (cmd == "clear") {
        return send_command("CLEAR");

    } else if (cmd == "dnd") {
        if (argc < 3) {
            // Show current status.
            return send_command("DND status");
        }

        std::string subcmd = argv[2];
        if (subcmd == "on" || subcmd == "off") {
            return send_command("DND " + subcmd);
        } else if (subcmd == "schedule") {
            if (argc >= 7) {
                // Set schedule: dnd schedule <start_h> <start_m> <end_h> <end_m>
                std::ostringstream oss;
                oss << "DND schedule " << argv[3] << " " << argv[4]
                    << " " << argv[5] << " " << argv[6];
                return send_command(oss.str());
            }
            // Show current schedule.
            return send_command("DND schedule");
        } else {
            std::cerr << "Usage: straylight-notify-cli dnd [on|off|schedule]\n";
            return 1;
        }

    } else if (cmd == "rules") {
        if (argc < 3) {
            // List rules by default.
            return send_command("RULE list");
        }

        std::string subcmd = argv[2];
        if (subcmd == "list") {
            return send_command("RULE list");
        } else if (subcmd == "add") {
            if (argc < 5) {
                std::cerr << "Usage: straylight-notify-cli rules add <app-pattern> <action>\n";
                std::cerr << "Actions: show, suppress, modify, redirect\n";
                return 1;
            }
            std::string pattern = argv[3];
            std::string action = argv[4];
            return send_command("RULE add " + pattern + " " + action);
        } else if (subcmd == "remove") {
            if (argc < 4) {
                std::cerr << "Usage: straylight-notify-cli rules remove <id>\n";
                return 1;
            }
            return send_command("RULE remove " + std::string(argv[3]));
        } else {
            std::cerr << "Unknown rules command: " << subcmd << "\n";
            return 1;
        }

    } else {
        std::cerr << "Error: unknown command '" << cmd << "'\n";
        print_usage();
        return 1;
    }

    return 0;
}
