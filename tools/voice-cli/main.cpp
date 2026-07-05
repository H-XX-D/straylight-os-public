#include <cerrno>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static constexpr const char* kSocketPath = "/run/straylight/voice.sock";

static void usage() {
    std::cerr
        << "straylight-voice-cli - control the StrayLight voice daemon\n\n"
        << "Usage:\n"
        << "  straylight-voice-cli status\n"
        << "  straylight-voice-cli ask <text>\n"
        << "  straylight-voice-cli say <text>\n"
        << "  straylight-voice-cli models\n"
        << "  straylight-voice-cli config\n"
        << "  straylight-voice-cli history\n"
        << "  straylight-voice-cli clear\n";
}

static std::string json_escape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 16);
    for (char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

static std::string join_args(int argc, char* argv[], int start) {
    std::ostringstream out;
    for (int i = start; i < argc; ++i) {
        if (i > start) out << ' ';
        out << argv[i];
    }
    return out.str();
}

static int rpc(const std::string& method, const std::string& params_json) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        std::cerr << "Error: socket() failed: " << std::strerror(errno) << "\n";
        return 1;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, kSocketPath, sizeof(addr.sun_path) - 1);

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "Error: cannot connect to " << kSocketPath
                  << " (" << std::strerror(errno) << ")\n";
        ::close(fd);
        return 1;
    }

    std::ostringstream req;
    req << "{\"jsonrpc\":\"2.0\",\"method\":\"" << method
        << "\",\"params\":" << params_json << ",\"id\":1}\n";
    std::string payload = req.str();

    ssize_t written = ::write(fd, payload.data(), payload.size());
    if (written < 0 || static_cast<size_t>(written) != payload.size()) {
        std::cerr << "Error: write failed: " << std::strerror(errno) << "\n";
        ::close(fd);
        return 1;
    }

    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    int ready = ::poll(&pfd, 1, 10000);
    if (ready <= 0) {
        std::cerr << "Error: timed out waiting for voice daemon\n";
        ::close(fd);
        return 1;
    }

    std::string response;
    char buf[8192];
    while (true) {
        ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
        if (n <= 0) break;
        buf[n] = '\0';
        response += buf;
        if (response.find('\n') != std::string::npos) break;
    }
    ::close(fd);

    if (response.empty()) {
        std::cerr << "Error: empty response from voice daemon\n";
        return 1;
    }

    std::cout << response;
    if (response.back() != '\n') std::cout << '\n';
    return response.find("\"error\"") == std::string::npos ? 0 : 1;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        usage();
        return 1;
    }

    std::string command = argv[1];
    if (command == "-h" || command == "--help") {
        usage();
        return 0;
    }

    if (command == "status") return rpc("voice.status", "{}");
    if (command == "models") return rpc("voice.models", "{}");
    if (command == "config") return rpc("voice.config", "{}");
    if (command == "history") return rpc("voice.history", "{}");
    if (command == "clear") return rpc("voice.clear", "{}");

    if (command == "ask" || command == "say") {
        if (argc < 3) {
            std::cerr << "Error: missing text\n";
            return 1;
        }
        std::string text = join_args(argc, argv, 2);
        std::string params = "{\"text\":\"" + json_escape(text) + "\"}";
        return rpc(command == "ask" ? "voice.ask" : "voice.say", params);
    }

    std::cerr << "Error: unknown command: " << command << "\n";
    usage();
    return 1;
}
