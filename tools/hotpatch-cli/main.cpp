/**
 * straylight-hotpatch — CLI for the StrayLight live-patching system.
 *
 * Commands:
 *   apply kernel <module> <patch.ko>   Apply a kernel livepatch
 *   apply daemon <service> <data>      Hot-reload a daemon
 *   apply config <path> <diff_file>    Patch a config file live
 *   rollback <patch-id>                Undo a patch
 *   list [applied|rolled_back|failed]  List patches
 *   status <patch-id>                  Show patch details
 *   history                            Show all patches (alias for list)
 *
 * Communicates with straylight-hotpatchd via Unix socket.
 */

#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

static constexpr const char* SOCKET_PATH =
    "/var/run/straylight/hotpatch.sock";

static std::string send_command(const std::string& cmd) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return "ERR cannot create socket: " + std::string(strerror(errno));
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return "ERR cannot connect to hotpatch daemon: " +
               std::string(strerror(errno)) +
               "\n  Is straylight-hotpatchd running?";
    }

    std::string wire = cmd + "\n";
    ssize_t written = write(fd, wire.data(), wire.size());
    if (written < 0) {
        close(fd);
        return "ERR write failed";
    }

    // Shutdown write side so daemon sees EOF
    ::shutdown(fd, SHUT_WR);

    // Read response
    std::string response;
    char buf[4096];
    while (true) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        response.append(buf, static_cast<size_t>(n));
    }
    close(fd);

    // Trim trailing newline
    while (!response.empty() &&
           (response.back() == '\n' || response.back() == '\r'))
        response.pop_back();

    return response;
}

static std::string read_file_content(const std::string& path) {
    std::ifstream in(path);
    if (!in) return "";
    return std::string((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
}

static void print_usage() {
    std::cout
        << "Usage: straylight-hotpatch <command> [args...]\n"
        << "\n"
        << "Commands:\n"
        << "  apply kernel <module> <patch.ko>      Apply kernel livepatch\n"
        << "  apply daemon <service> <patch_data>    Hot-reload a daemon\n"
        << "  apply config <path> <diff_file>        Live-patch a config file\n"
        << "  rollback <patch-id>                    Undo a patch\n"
        << "  list [applied|rolled_back|failed]      List patches\n"
        << "  status <patch-id>                      Show patch details\n"
        << "  history                                Show all patches\n"
        << "\n"
        << "Examples:\n"
        << "  straylight-hotpatch apply kernel my_module /path/to/fix.ko\n"
        << "  straylight-hotpatch apply config /etc/straylight/quota/quotas.conf fix.diff\n"
        << "  straylight-hotpatch rollback hp-3a7f2b1c\n"
        << "  straylight-hotpatch list applied\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string command = argv[1];

    if (command == "apply") {
        if (argc < 4) {
            std::cerr << "Error: apply requires <type> <target> [data]\n";
            print_usage();
            return 1;
        }

        std::string type = argv[2];

        if (type == "kernel") {
            if (argc < 5) {
                std::cerr << "Error: apply kernel requires <module> <patch_path>\n";
                return 1;
            }
            std::string cmd = "APPLY_KERNEL " + std::string(argv[3]) + " " +
                              std::string(argv[4]);
            std::cout << send_command(cmd) << "\n";
        } else if (type == "daemon") {
            if (argc < 5) {
                std::cerr << "Error: apply daemon requires <service> <patch_data>\n";
                return 1;
            }
            std::string cmd = "APPLY_DAEMON " + std::string(argv[3]) + " " +
                              std::string(argv[4]);
            std::cout << send_command(cmd) << "\n";
        } else if (type == "config") {
            if (argc < 5) {
                std::cerr << "Error: apply config requires <config_path> <diff_file>\n";
                return 1;
            }
            std::string config_path = argv[3];
            std::string diff_file = argv[4];
            std::string diff_content = read_file_content(diff_file);
            if (diff_content.empty()) {
                std::cerr << "Error: cannot read diff file: " << diff_file << "\n";
                return 1;
            }
            std::string cmd = "APPLY_CONFIG " + config_path + " " + diff_content;
            std::cout << send_command(cmd) << "\n";
        } else {
            std::cerr << "Error: unknown patch type '" << type << "'\n"
                      << "  Valid types: kernel, daemon, config\n";
            return 1;
        }
    } else if (command == "rollback") {
        if (argc < 3) {
            std::cerr << "Error: rollback requires <patch-id>\n";
            return 1;
        }
        std::string cmd = "ROLLBACK " + std::string(argv[2]);
        std::cout << send_command(cmd) << "\n";
    } else if (command == "list" || command == "history") {
        std::string filter = (argc > 2) ? argv[2] : "";
        std::string cmd = "LIST" + (filter.empty() ? "" : " " + filter);
        std::cout << send_command(cmd) << "\n";
    } else if (command == "status") {
        if (argc < 3) {
            std::cerr << "Error: status requires <patch-id>\n";
            return 1;
        }
        std::string cmd = "STATUS " + std::string(argv[2]);
        std::cout << send_command(cmd) << "\n";
    } else if (command == "--help" || command == "-h" || command == "help") {
        print_usage();
    } else {
        std::cerr << "Error: unknown command '" << command << "'\n";
        print_usage();
        return 1;
    }

    return 0;
}
