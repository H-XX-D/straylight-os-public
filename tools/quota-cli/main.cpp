/**
 * straylight-quota — CLI for the StrayLight resource quota system.
 *
 * Commands:
 *   set <app> <pid> --cpu=N --ram=N --vram=N --gpu=N --iops=N --net=N --fps=N
 *   get <app>                        Show quota + usage
 *   list                             List all quotas
 *   usage                            Full usage report
 *   enforce [on|off]                 Enable/disable enforcement
 *   violations [count]               Show recent violations
 */

#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

static constexpr const char* SOCKET_PATH =
    "/var/run/straylight/quota.sock";

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
        return "ERR cannot connect to quota daemon: " +
               std::string(strerror(errno)) +
               "\n  Is straylight-quotad running?";
    }

    std::string wire = cmd + "\n";
    ssize_t written = write(fd, wire.data(), wire.size());
    if (written < 0) {
        close(fd);
        return "ERR write failed";
    }

    ::shutdown(fd, SHUT_WR);

    std::string response;
    char buf[4096];
    while (true) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        response.append(buf, static_cast<size_t>(n));
    }
    close(fd);

    while (!response.empty() &&
           (response.back() == '\n' || response.back() == '\r'))
        response.pop_back();

    return response;
}

/** Parse --key=value into key=value for the daemon protocol. */
static std::string normalize_flag(const std::string& arg) {
    std::string s = arg;
    // Strip leading dashes
    while (!s.empty() && s.front() == '-') s = s.substr(1);
    // Remap friendly names to daemon keys
    auto eq = s.find('=');
    if (eq != std::string::npos) {
        std::string key = s.substr(0, eq);
        std::string val = s.substr(eq + 1);
        if (key == "iops") key = "disk_iops";
        return key + "=" + val;
    }
    return s;
}

static void print_usage() {
    std::cout
        << "Usage: straylight-quota <command> [args...]\n"
        << "\n"
        << "Commands:\n"
        << "  set <app> <pid> [--cpu=N] [--ram=N] [--vram=N] [--gpu=N]\n"
        << "                  [--iops=N] [--net=N] [--fps=N]\n"
        << "                                 Set resource quotas for an app\n"
        << "  get <app>                      Show quota and current usage\n"
        << "  list                           List all apps with quotas\n"
        << "  usage                          Full usage report\n"
        << "  enforce [on|off]               Enable/disable enforcement\n"
        << "  violations [count]             Show recent violations\n"
        << "\n"
        << "Resource units:\n"
        << "  cpu   — percentage (e.g., 50 = 50%% of one core)\n"
        << "  ram   — bytes (e.g., 4294967296 = 4 GiB)\n"
        << "  vram  — bytes (VPU VRAM budget)\n"
        << "  gpu   — percentage (mesh GPU compute)\n"
        << "  iops  — I/O operations per second\n"
        << "  net   — bytes/second bandwidth\n"
        << "  fps   — compositor frames per second cap\n"
        << "\n"
        << "Examples:\n"
        << "  straylight-quota set firefox 1234 --cpu=50 --ram=4294967296 --vram=1073741824\n"
        << "  straylight-quota get firefox\n"
        << "  straylight-quota usage\n"
        << "  straylight-quota enforce off\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string command = argv[1];

    if (command == "set") {
        if (argc < 4) {
            std::cerr << "Error: set requires <app> <pid> [--key=value ...]\n";
            print_usage();
            return 1;
        }

        std::string app = argv[2];
        std::string pid = argv[3];

        std::ostringstream cmd;
        cmd << "SET " << app << " " << pid;
        for (int i = 4; i < argc; ++i) {
            cmd << " " << normalize_flag(argv[i]);
        }
        std::cout << send_command(cmd.str()) << "\n";

    } else if (command == "get") {
        if (argc < 3) {
            std::cerr << "Error: get requires <app>\n";
            return 1;
        }
        std::cout << send_command("GET " + std::string(argv[2])) << "\n";

    } else if (command == "list") {
        std::cout << send_command("LIST") << "\n";

    } else if (command == "usage") {
        std::cout << send_command("USAGE") << "\n";

    } else if (command == "enforce") {
        if (argc < 3) {
            std::cerr << "Error: enforce requires [on|off]\n";
            return 1;
        }
        std::cout << send_command("ENFORCE " + std::string(argv[2])) << "\n";

    } else if (command == "violations") {
        std::string count = (argc > 2) ? argv[2] : "50";
        std::cout << send_command("VIOLATIONS " + count) << "\n";

    } else if (command == "--help" || command == "-h" || command == "help") {
        print_usage();
    } else {
        std::cerr << "Error: unknown command '" << command << "'\n";
        print_usage();
        return 1;
    }

    return 0;
}
