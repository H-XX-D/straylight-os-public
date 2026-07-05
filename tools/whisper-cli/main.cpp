// tools/whisper-cli/main.cpp
// CLI front-end for straylight-whisper — encrypted inter-process messaging.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static constexpr const char* kSocketPath = "/run/straylight/whisper.sock";

static void print_usage() {
    std::cerr
        << "straylight-whisper — encrypted inter-process messaging\n"
        << "\n"
        << "Usage:\n"
        << "  straylight-whisper create <channel> [--key-exchange ecdh]\n"
        << "  straylight-whisper send <channel> <message>\n"
        << "  straylight-whisper recv <channel> [--timeout 30]\n"
        << "  straylight-whisper listen <channel>            Continuous receive\n"
        << "  straylight-whisper list                        List channels\n"
        << "  straylight-whisper delete <channel>            Remove channel\n"
        << "  straylight-whisper keyexchange <hex-pubkey>    Perform ECDH\n";
}

/// Connect to the Whisper daemon socket.
static int connect_daemon() {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        std::cerr << "Error: socket() failed: " << strerror(errno) << "\n";
        return -1;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, kSocketPath, sizeof(addr.sun_path) - 1);

    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr),
                  sizeof(addr)) < 0) {
        ::close(fd);
        std::cerr << "Error: cannot connect to whisper daemon at "
                  << kSocketPath << ": " << strerror(errno) << "\n";
        return -1;
    }
    return fd;
}

/// Send a length-prefixed message.
static bool send_msg(int fd, const std::string& msg) {
    uint32_t len = static_cast<uint32_t>(msg.size());
    if (::send(fd, &len, 4, 0) != 4) return false;
    if (::send(fd, msg.data(), len, 0) != static_cast<ssize_t>(len))
        return false;
    return true;
}

/// Receive a length-prefixed message.
static std::string recv_msg(int fd) {
    uint32_t len = 0;
    ssize_t n = ::recv(fd, &len, 4, MSG_WAITALL);
    if (n != 4) return {};
    if (len > 1024 * 1024) return {};

    std::string buf(len, '\0');
    n = ::recv(fd, buf.data(), len, MSG_WAITALL);
    if (n != static_cast<ssize_t>(len)) return {};
    return buf;
}

/// Send a command and print the response.
static int command(const std::string& cmd) {
    int fd = connect_daemon();
    if (fd < 0) return 1;

    if (!send_msg(fd, cmd)) {
        std::cerr << "Error: failed to send command\n";
        ::close(fd);
        return 1;
    }

    std::string response = recv_msg(fd);
    ::close(fd);

    if (response.empty()) {
        std::cerr << "Error: no response from daemon\n";
        return 1;
    }

    // Check if response starts with "ERR".
    if (response.substr(0, 3) == "ERR") {
        std::cerr << "Error: " << response.substr(4) << "\n";
        return 1;
    }

    // Strip "OK " prefix for display.
    if (response.size() > 3 && response.substr(0, 3) == "OK ") {
        std::cout << response.substr(3) << "\n";
    } else if (response == "OK") {
        // Silent success.
    } else {
        std::cout << response << "\n";
    }
    return 0;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string cmd_name = argv[1];
    if (cmd_name == "--help" || cmd_name == "-h") {
        print_usage();
        return 0;
    }

    // -----------------------------------------------------------------------
    // create
    // -----------------------------------------------------------------------
    if (cmd_name == "create") {
        if (argc < 3) {
            std::cerr << "Error: 'create' requires <channel>\n";
            return 1;
        }
        return command("CREATE " + std::string(argv[2]));
    }

    // -----------------------------------------------------------------------
    // send
    // -----------------------------------------------------------------------
    if (cmd_name == "send") {
        if (argc < 4) {
            std::cerr << "Error: 'send' requires <channel> <message>\n";
            return 1;
        }
        std::string channel = argv[2];
        // Join remaining args as the message.
        std::string message;
        for (int i = 3; i < argc; ++i) {
            if (!message.empty()) message += ' ';
            message += argv[i];
        }
        return command("SEND " + channel + " " + message);
    }

    // -----------------------------------------------------------------------
    // recv
    // -----------------------------------------------------------------------
    if (cmd_name == "recv") {
        if (argc < 3) {
            std::cerr << "Error: 'recv' requires <channel>\n";
            return 1;
        }
        std::string channel = argv[2];
        int timeout = 0;
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
                timeout = std::atoi(argv[++i]);
            }
        }

        if (timeout <= 0) {
            return command("RECV " + channel);
        }

        // Polling with timeout.
        auto start = std::chrono::steady_clock::now();
        while (true) {
            int fd = connect_daemon();
            if (fd < 0) return 1;
            send_msg(fd, "RECV " + channel);
            std::string response = recv_msg(fd);
            ::close(fd);

            if (!response.empty() && response.substr(0, 2) == "OK") {
                if (response.size() > 3) {
                    std::cout << response.substr(3) << "\n";
                }
                return 0;
            }

            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::steady_clock::now() - start)
                               .count();
            if (elapsed >= timeout) {
                std::cerr << "Timeout: no message received within "
                          << timeout << "s\n";
                return 1;
            }
            usleep(100000); // 100ms between polls.
        }
    }

    // -----------------------------------------------------------------------
    // listen
    // -----------------------------------------------------------------------
    if (cmd_name == "listen") {
        if (argc < 3) {
            std::cerr << "Error: 'listen' requires <channel>\n";
            return 1;
        }
        std::string channel = argv[2];
        std::cout << "Listening on channel '" << channel << "' (Ctrl+C to stop)\n";

        while (true) {
            int fd = connect_daemon();
            if (fd < 0) {
                usleep(1000000); // 1s retry.
                continue;
            }
            send_msg(fd, "RECV " + channel);
            std::string response = recv_msg(fd);
            ::close(fd);

            if (!response.empty() && response.substr(0, 2) == "OK" &&
                response.size() > 3) {
                std::cout << response.substr(3) << "\n";
            } else {
                usleep(100000); // 100ms between polls when empty.
            }
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // list
    // -----------------------------------------------------------------------
    if (cmd_name == "list") {
        return command("LIST");
    }

    // -----------------------------------------------------------------------
    // delete
    // -----------------------------------------------------------------------
    if (cmd_name == "delete") {
        if (argc < 3) {
            std::cerr << "Error: 'delete' requires <channel>\n";
            return 1;
        }
        return command("DELETE " + std::string(argv[2]));
    }

    // -----------------------------------------------------------------------
    // keyexchange
    // -----------------------------------------------------------------------
    if (cmd_name == "keyexchange") {
        if (argc < 3) {
            std::cerr << "Error: 'keyexchange' requires <hex-pubkey>\n";
            return 1;
        }
        return command("KEYEXCHANGE " + std::string(argv[2]));
    }

    std::cerr << "Error: unknown command '" << cmd_name << "'\n";
    print_usage();
    return 1;
}
