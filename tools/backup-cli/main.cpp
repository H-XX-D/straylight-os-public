// tools/backup-cli/main.cpp
// CLI front-end for straylight-backup daemon.
#include <straylight/ipc.h>

#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>

static const char* SOCKET_PATH = "/run/straylight/backup.sock";

static void print_usage() {
    std::cerr
        << "straylight-backup — backup scheduler CLI\n"
        << "\n"
        << "Usage:\n"
        << "  straylight-backup now [--schedule <name>]   Run a backup now\n"
        << "  straylight-backup schedule                  Show backup schedules\n"
        << "  straylight-backup list                      List all backups\n"
        << "  straylight-backup restore <id> [--to <dir>] Restore a backup\n"
        << "  straylight-backup verify <id>               Verify backup integrity\n"
        << "  straylight-backup remote add <name> <host> <path>\n"
        << "  straylight-backup remote remove <name>\n"
        << "  straylight-backup remote list\n";
}

static nlohmann::json send_command(const std::string& cmd,
                                    const nlohmann::json& params = {}) {
    straylight::IpcClient client;
    auto connect = client.connect(SOCKET_PATH);
    if (!connect.has_value()) {
        std::cerr << "Error: cannot connect to backup daemon: " << connect.error() << "\n";
        std::exit(1);
    }

    timeval tv{};
    tv.tv_sec = 5;
    setsockopt(client.fd(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    nlohmann::json msg;
    msg["cmd"] = cmd;
    if (!params.empty()) {
        msg["params"] = params;
    }

    auto send = client.send(msg.dump());
    if (!send.has_value()) {
        std::cerr << "Error: " << send.error() << "\n";
        std::exit(1);
    }

    auto recv = client.receive();
    if (!recv.has_value()) {
        std::cerr << "Error: " << recv.error() << "\n";
        std::exit(1);
    }

    try {
        return nlohmann::json::parse(recv.value());
    } catch (const nlohmann::json::parse_error& e) {
        std::cerr << "Error: daemon returned invalid JSON: " << e.what() << "\n";
        std::exit(1);
    }
}

static std::string format_size(uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit = 0;
    double size = static_cast<double>(bytes);
    while (size >= 1024.0 && unit < 4) {
        size /= 1024.0;
        unit++;
    }
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << size << " " << units[unit];
    return oss.str();
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
    // now
    // -----------------------------------------------------------------------
    if (command == "now") {
        nlohmann::json params;
        for (int i = 2; i < argc; ++i) {
            if (std::strcmp(argv[i], "--schedule") == 0 && i + 1 < argc) {
                params["schedule"] = argv[++i];
            }
        }
        std::cout << "Running backup...\n";
        auto resp = send_command("now", params);
        if (resp.value("status", "") == "ok") {
            std::cout << "\033[32mBackup complete.\033[0m\n"
                      << "  ID:   " << resp.value("backup_id", "") << "\n"
                      << "  Size: " << format_size(resp.value("size", uint64_t(0))) << "\n";
        } else {
            std::cerr << "Error: " << resp.value("message", "unknown") << "\n";
            return 1;
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // schedule
    // -----------------------------------------------------------------------
    if (command == "schedule") {
        auto resp = send_command("schedule");
        if (resp.value("status", "") != "ok") {
            std::cerr << "Error: " << resp.value("message", "unknown") << "\n";
            return 1;
        }
        auto& schedules = resp["schedules"];
        if (schedules.empty()) {
            std::cout << "No schedules configured.\n";
            return 0;
        }
        printf("%-20s %-30s %-10s %-8s\n", "NAME", "SOURCE", "SCHEDULE", "ENABLED");
        for (const auto& s : schedules) {
            printf("%-20s %-30s %-10s %-8s\n",
                   s.value("name", "").c_str(),
                   s.value("source", "").c_str(),
                   s.value("schedule", "").c_str(),
                   s.value("enabled", false) ? "yes" : "no");
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
        auto& backups = resp["backups"];
        if (backups.empty()) {
            std::cout << "No backups found.\n";
            return 0;
        }
        printf("%-25s %-10s %-20s %-10s %-8s %-8s\n",
               "ID", "TYPE", "SOURCE", "SIZE", "ENC", "STATUS");
        for (const auto& b : backups) {
            std::string enc = b.value("encrypted", false) ? "yes" : "no";
            std::string status = b.value("status", "");
            std::string color = status == "ok" ? "\033[32m" : "\033[31m";
            printf("%-25s %-10s %-20s %-10s %-8s %s%-8s\033[0m\n",
                   b.value("id", "").c_str(),
                   b.value("type", "").c_str(),
                   b.value("source", "").c_str(),
                   format_size(b.value("size_bytes", uint64_t(0))).c_str(),
                   enc.c_str(),
                   color.c_str(),
                   status.c_str());
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // restore <id>
    // -----------------------------------------------------------------------
    if (command == "restore") {
        if (argc < 3) {
            std::cerr << "Error: 'restore' requires a backup ID\n";
            return 1;
        }
        nlohmann::json params;
        params["id"] = argv[2];
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "--to") == 0 && i + 1 < argc) {
                params["target"] = argv[++i];
            }
        }
        std::cout << "Restoring backup " << argv[2] << "...\n";
        auto resp = send_command("restore", params);
        if (resp.value("status", "") == "ok") {
            std::cout << "\033[32mRestore complete.\033[0m\n";
        } else {
            std::cerr << "Error: " << resp.value("message", "unknown") << "\n";
            return 1;
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // verify <id>
    // -----------------------------------------------------------------------
    if (command == "verify") {
        if (argc < 3) {
            std::cerr << "Error: 'verify' requires a backup ID\n";
            return 1;
        }
        nlohmann::json params;
        params["id"] = argv[2];
        auto resp = send_command("verify", params);
        if (resp.value("status", "") == "ok") {
            bool valid = resp.value("valid", false);
            if (valid) {
                std::cout << "\033[32mBackup " << argv[2] << " is valid.\033[0m\n";
            } else {
                std::cout << "\033[31mBackup " << argv[2] << " is CORRUPT.\033[0m\n";
                return 1;
            }
        } else {
            std::cerr << "Error: " << resp.value("message", "unknown") << "\n";
            return 1;
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // remote add/remove/list
    // -----------------------------------------------------------------------
    if (command == "remote") {
        if (argc < 3) {
            std::cerr << "Error: 'remote' requires a subcommand (add/remove/list)\n";
            return 1;
        }
        std::string sub = argv[2];

        if (sub == "add") {
            if (argc < 6) {
                std::cerr << "Usage: straylight-backup remote add <name> <host> <path>\n";
                return 1;
            }
            nlohmann::json params;
            params["name"] = argv[3];
            params["host"] = argv[4];
            params["path"] = argv[5];
            auto resp = send_command("remote-add", params);
            if (resp.value("status", "") == "ok") {
                std::cout << "Remote '" << argv[3] << "' added.\n";
            } else {
                std::cerr << "Error: " << resp.value("message", "unknown") << "\n";
                return 1;
            }
        } else if (sub == "remove") {
            if (argc < 4) {
                std::cerr << "Usage: straylight-backup remote remove <name>\n";
                return 1;
            }
            nlohmann::json params;
            params["name"] = argv[3];
            auto resp = send_command("remote-remove", params);
            if (resp.value("status", "") == "ok") {
                std::cout << "Remote '" << argv[3] << "' removed.\n";
            } else {
                std::cerr << "Error: " << resp.value("message", "unknown") << "\n";
                return 1;
            }
        } else if (sub == "list") {
            auto resp = send_command("remote-list");
            if (resp.value("status", "") != "ok") {
                std::cerr << "Error: " << resp.value("message", "unknown") << "\n";
                return 1;
            }
            auto& remotes = resp["remotes"];
            if (remotes.empty()) {
                std::cout << "No remote targets configured.\n";
            } else {
                printf("%-15s %-25s %-30s %-6s\n", "NAME", "HOST", "PATH", "PORT");
                for (const auto& r : remotes) {
                    printf("%-15s %-25s %-30s %-6d\n",
                           r.value("name", "").c_str(),
                           r.value("host", "").c_str(),
                           r.value("path", "").c_str(),
                           r.value("port", 22));
                }
            }
        } else {
            std::cerr << "Unknown remote subcommand: " << sub << "\n";
            return 1;
        }
        return 0;
    }

    std::cerr << "Unknown command: " << command << "\n";
    print_usage();
    return 1;
}
