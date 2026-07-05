/**
 * straylight-fuse — CLI for the StrayLight process fusion system.
 *
 * Commands:
 *   fuse <pid1> <pid2> [--size=N]    Create a fusion session
 *   list                              List active sessions
 *   destroy <session-id>              Tear down a session
 *   stats                             Show aggregate statistics
 *   get <session-id>                  Show session details
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
    "/var/run/straylight/fuse.sock";

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
        return "ERR cannot connect to fuse daemon: " +
               std::string(strerror(errno)) +
               "\n  Is straylight-fused running?";
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

static size_t parse_size(const std::string& s) {
    size_t val = std::stoull(s);
    // Support K/M/G suffixes
    if (!s.empty()) {
        char last = s.back();
        if (last == 'K' || last == 'k') val = std::stoull(s.substr(0, s.size()-1)) * 1024;
        else if (last == 'M' || last == 'm') val = std::stoull(s.substr(0, s.size()-1)) * 1024 * 1024;
        else if (last == 'G' || last == 'g') val = std::stoull(s.substr(0, s.size()-1)) * 1024 * 1024 * 1024;
    }
    return val;
}

static void print_usage() {
    std::cout
        << "Usage: straylight-fuse <command> [args...]\n"
        << "\n"
        << "Commands:\n"
        << "  fuse <pid1> <pid2> [--size=N]    Create fusion (default 4M shared region)\n"
        << "  list                              List active fusion sessions\n"
        << "  destroy <session-id>              Tear down a session\n"
        << "  stats                             Aggregate statistics\n"
        << "  get <session-id>                  Session details\n"
        << "\n"
        << "Size suffixes: K, M, G (e.g., --size=16M)\n"
        << "\n"
        << "Examples:\n"
        << "  straylight-fuse fuse 1234 5678 --size=8M\n"
        << "  straylight-fuse list\n"
        << "  straylight-fuse destroy fuse-1\n"
        << "  straylight-fuse stats\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string command = argv[1];

    if (command == "fuse") {
        if (argc < 4) {
            std::cerr << "Error: fuse requires <pid1> <pid2>\n";
            print_usage();
            return 1;
        }

        std::string pid1 = argv[2];
        std::string pid2 = argv[3];

        // Parse --size=N option
        size_t region_size = 4 * 1024 * 1024; // 4 MiB default
        for (int i = 4; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg.substr(0, 7) == "--size=") {
                try {
                    region_size = parse_size(arg.substr(7));
                } catch (...) {
                    std::cerr << "Error: invalid size: " << arg.substr(7) << "\n";
                    return 1;
                }
            }
        }

        std::string cmd = "FUSE " + pid1 + " " + pid2 + " " +
                          std::to_string(region_size);
        std::cout << send_command(cmd) << "\n";

    } else if (command == "list") {
        std::cout << send_command("LIST") << "\n";

    } else if (command == "destroy") {
        if (argc < 3) {
            std::cerr << "Error: destroy requires <session-id>\n";
            return 1;
        }
        std::cout << send_command("DESTROY " + std::string(argv[2])) << "\n";

    } else if (command == "stats") {
        std::cout << send_command("STATS") << "\n";

    } else if (command == "get") {
        if (argc < 3) {
            std::cerr << "Error: get requires <session-id>\n";
            return 1;
        }
        std::cout << send_command("GET " + std::string(argv[2])) << "\n";

    } else if (command == "--help" || command == "-h" || command == "help") {
        print_usage();
    } else {
        std::cerr << "Error: unknown command '" << command << "'\n";
        print_usage();
        return 1;
    }

    return 0;
}
