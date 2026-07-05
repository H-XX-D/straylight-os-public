/**
 * StrayLight Hotpatch Daemon
 *
 * Listens on a Unix domain socket for patch commands:
 *   APPLY_KERNEL <module> <patch_path>
 *   APPLY_DAEMON <service> <patch_data>
 *   APPLY_CONFIG <config_path> <diff>
 *   ROLLBACK <patch_id>
 *   LIST [status_filter]
 *   STATUS <patch_id>
 *
 * Responses are newline-terminated text: OK <data> or ERR <message>
 */

#include "patch_engine.h"
#include "patch_registry.h"
#include "straylight/daemon_base.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

static constexpr const char* SOCKET_PATH =
    "/var/run/straylight/hotpatch.sock";

namespace {

/** Split a string by the first N spaces, leaving the rest as the last token. */
std::vector<std::string> split_cmd(const std::string& s, size_t max_parts) {
    std::vector<std::string> parts;
    size_t pos = 0;
    while (parts.size() + 1 < max_parts) {
        while (pos < s.size() && s[pos] == ' ') ++pos;
        if (pos >= s.size()) break;
        size_t end = s.find(' ', pos);
        if (end == std::string::npos) { end = s.size(); }
        parts.push_back(s.substr(pos, end - pos));
        pos = end;
    }
    // Rest goes as final token
    while (pos < s.size() && s[pos] == ' ') ++pos;
    if (pos < s.size()) {
        parts.push_back(s.substr(pos));
    }
    return parts;
}

} // anonymous namespace

class HotpatchDaemon : public straylight::DaemonBase {
public:
    HotpatchDaemon()
        : DaemonBase("straylight-hotpatch")
        , registry_()
        , engine_(registry_)
    {}

protected:
    straylight::VoidResult<> init() override {
        set_tick_interval_ms(50);

        // Load existing registry
        auto r = registry_.load();
        if (!r) {
            fprintf(stderr, "[hotpatch] warning: registry load: %s\n",
                    r.err().c_str());
            // Non-fatal — might be first boot
        }

        // Create Unix socket
        listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
        if (listen_fd_ < 0) {
            return straylight::VoidResult<>::error(
                std::string("socket(): ") + strerror(errno));
        }

        // Remove stale socket file
        std::error_code ec;
        std::filesystem::remove(SOCKET_PATH, ec);

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

        // Ensure parent directory exists
        std::filesystem::create_directories(
            std::filesystem::path(SOCKET_PATH).parent_path(), ec);

        if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr),
                 sizeof(addr)) < 0) {
            close(listen_fd_);
            return straylight::VoidResult<>::error(
                std::string("bind(): ") + strerror(errno));
        }

        // Allow non-root clients
        chmod(SOCKET_PATH, 0666);

        if (listen(listen_fd_, 8) < 0) {
            close(listen_fd_);
            return straylight::VoidResult<>::error(
                std::string("listen(): ") + strerror(errno));
        }

        // Set non-blocking so tick() doesn't block forever
        int flags = fcntl(listen_fd_, F_GETFL, 0);
        fcntl(listen_fd_, F_SETFL, flags | O_NONBLOCK);

        return straylight::VoidResult<>::ok();
    }

    void tick() override {
        // Accept new connections (non-blocking)
        struct sockaddr_un client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_fd_,
            reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client_fd < 0) return; // EAGAIN — no pending connections

        // Handle client in a detached thread to avoid blocking the main loop
        std::thread([this, client_fd]() {
            handle_client(client_fd);
        }).detach();
    }

    void shutdown() override {
        if (listen_fd_ >= 0) {
            close(listen_fd_);
            listen_fd_ = -1;
        }
        std::error_code ec;
        std::filesystem::remove(SOCKET_PATH, ec);
    }

    void on_reload() override {
        registry_.load();
    }

private:
    straylight::hotpatch::PatchRegistry registry_;
    straylight::hotpatch::PatchEngine engine_;
    int listen_fd_ = -1;

    void handle_client(int fd) {
        // Read full command (up to 64 KiB)
        std::string cmd;
        char buf[4096];
        while (true) {
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n <= 0) break;
            cmd.append(buf, static_cast<size_t>(n));
            // Commands are newline-terminated
            if (cmd.find('\n') != std::string::npos) break;
            if (cmd.size() > 65536) break;
        }

        // Trim trailing newline
        while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r'))
            cmd.pop_back();

        std::string response = dispatch(cmd);
        response += "\n";
        ssize_t written = write(fd, response.data(), response.size());
        (void)written;
        close(fd);
    }

    std::string dispatch(const std::string& cmd) {
        auto parts = split_cmd(cmd, 4);
        if (parts.empty()) return "ERR empty command";

        std::string verb = parts[0];
        // Uppercase the verb
        std::transform(verb.begin(), verb.end(), verb.begin(), ::toupper);

        if (verb == "APPLY_KERNEL") {
            if (parts.size() < 3)
                return "ERR usage: APPLY_KERNEL <module> <patch_path>";
            auto r = engine_.apply_kernel_patch(parts[1], parts[2]);
            if (r) return "OK " + r.value();
            return "ERR " + r.err();
        }

        if (verb == "APPLY_DAEMON") {
            if (parts.size() < 3)
                return "ERR usage: APPLY_DAEMON <service> <patch_data>";
            auto r = engine_.apply_daemon_patch(parts[1], parts[2]);
            if (r) return "OK " + r.value();
            return "ERR " + r.err();
        }

        if (verb == "APPLY_CONFIG") {
            if (parts.size() < 3)
                return "ERR usage: APPLY_CONFIG <config_path> <diff>";
            auto r = engine_.apply_config_patch(parts[1], parts[2]);
            if (r) return "OK " + r.value();
            return "ERR " + r.err();
        }

        if (verb == "ROLLBACK") {
            if (parts.size() < 2) return "ERR usage: ROLLBACK <patch_id>";
            auto r = engine_.rollback(parts[1]);
            if (r) return "OK rolled back";
            return "ERR " + r.err();
        }

        if (verb == "LIST") {
            std::string filter = parts.size() > 1 ? parts[1] : "";
            auto records = engine_.list(filter);
            std::ostringstream ss;
            ss << "OK " << records.size() << " patches\n";
            for (const auto& rec : records) {
                ss << rec.patch_id << " "
                   << straylight::hotpatch::patch_type_str(rec.type) << " "
                   << rec.target << " "
                   << straylight::hotpatch::patch_status_str(rec.status) << " "
                   << rec.applied_at << "\n";
            }
            return ss.str();
        }

        if (verb == "STATUS") {
            if (parts.size() < 2) return "ERR usage: STATUS <patch_id>";
            auto r = engine_.status(parts[1]);
            if (!r) return "ERR " + r.err();
            const auto& rec = r.value();
            std::ostringstream ss;
            ss << "OK\n"
               << "patch_id:     " << rec.patch_id << "\n"
               << "type:         " << straylight::hotpatch::patch_type_str(rec.type) << "\n"
               << "target:       " << rec.target << "\n"
               << "patch_source: " << rec.patch_source << "\n"
               << "applied_at:   " << rec.applied_at << "\n"
               << "status:       " << straylight::hotpatch::patch_status_str(rec.status) << "\n"
               << "description:  " << rec.description;
            return ss.str();
        }

        return "ERR unknown command: " + verb;
    }
};

int main(int /*argc*/, char* /*argv*/[]) {
    HotpatchDaemon daemon;
    return daemon.run();
}
