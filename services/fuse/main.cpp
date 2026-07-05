/**
 * StrayLight Fuse Daemon — Process Fusion Manager
 *
 * Listens on a Unix domain socket for fusion commands:
 *   FUSE <pid1> <pid2> <size1> [size2 ...]   Create fusion session
 *   DESTROY <session_id>                       Tear down a session
 *   LIST                                       List active sessions
 *   STATS                                      Aggregate statistics
 *   GET <session_id>                           Session details
 *
 * Periodically monitors fused process health and reaps dead sessions.
 */

#include "fusion_engine.h"
#include "monitor.h"
#include "shared_region.h"
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
    "/var/run/straylight/fuse.sock";

namespace {

std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> parts;
    std::istringstream iss(s);
    std::string token;
    while (iss >> token) parts.push_back(token);
    return parts;
}

} // anonymous namespace

class FuseDaemon : public straylight::DaemonBase {
public:
    FuseDaemon()
        : DaemonBase("straylight-fuse")
        , region_mgr_()
        , engine_(region_mgr_)
        , monitor_(engine_)
    {}

protected:
    straylight::VoidResult<> init() override {
        set_tick_interval_ms(100);

        // Set up monitor alerts
        monitor_.on_alert([](const straylight::fuse::MonitorAlert& alert) {
            fprintf(stderr, "[fuse-monitor] %s\n", alert.message.c_str());
        });

        // Create Unix socket
        listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
        if (listen_fd_ < 0) {
            return straylight::VoidResult<>::error(
                std::string("socket(): ") + strerror(errno));
        }

        std::error_code ec;
        std::filesystem::remove(SOCKET_PATH, ec);
        std::filesystem::create_directories(
            std::filesystem::path(SOCKET_PATH).parent_path(), ec);

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

        if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr),
                 sizeof(addr)) < 0) {
            close(listen_fd_);
            return straylight::VoidResult<>::error(
                std::string("bind(): ") + strerror(errno));
        }

        chmod(SOCKET_PATH, 0666);

        if (listen(listen_fd_, 8) < 0) {
            close(listen_fd_);
            return straylight::VoidResult<>::error(
                std::string("listen(): ") + strerror(errno));
        }

        int flags = fcntl(listen_fd_, F_GETFL, 0);
        fcntl(listen_fd_, F_SETFL, flags | O_NONBLOCK);

        return straylight::VoidResult<>::ok();
    }

    void tick() override {
        // Accept connections
        struct sockaddr_un client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_fd_,
            reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client_fd >= 0) {
            std::thread([this, client_fd]() {
                handle_client(client_fd);
            }).detach();
        }

        // Run health monitor every 10 ticks (~1 second)
        if (++tick_count_ % 10 == 0) {
            monitor_.check();
        }
    }

    void shutdown() override {
        if (listen_fd_ >= 0) {
            close(listen_fd_);
            listen_fd_ = -1;
        }
        std::error_code ec;
        std::filesystem::remove(SOCKET_PATH, ec);
    }

private:
    straylight::fuse::SharedRegionManager region_mgr_;
    straylight::fuse::FusionEngine engine_;
    straylight::fuse::FuseMonitor monitor_;
    int listen_fd_ = -1;
    uint64_t tick_count_ = 0;

    void handle_client(int fd) {
        std::string cmd;
        char buf[4096];
        while (true) {
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n <= 0) break;
            cmd.append(buf, static_cast<size_t>(n));
            if (cmd.find('\n') != std::string::npos) break;
            if (cmd.size() > 65536) break;
        }

        while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r'))
            cmd.pop_back();

        std::string response = dispatch(cmd);
        response += "\n";
        ssize_t written = write(fd, response.data(), response.size());
        (void)written;
        close(fd);
    }

    std::string dispatch(const std::string& cmd) {
        auto parts = split(cmd);
        if (parts.empty()) return "ERR empty command";

        std::string verb = parts[0];
        std::transform(verb.begin(), verb.end(), verb.begin(), ::toupper);

        if (verb == "FUSE") {
            if (parts.size() < 3)
                return "ERR usage: FUSE <pid1> <pid2> [size1 size2 ...]";

            pid_t pid1 = 0, pid2 = 0;
            try {
                pid1 = static_cast<pid_t>(std::stoi(parts[1]));
                pid2 = static_cast<pid_t>(std::stoi(parts[2]));
            } catch (...) {
                return "ERR invalid PID";
            }

            // Parse region sizes (default: one 4 MiB region)
            std::vector<size_t> sizes;
            for (size_t i = 3; i < parts.size(); ++i) {
                try {
                    sizes.push_back(std::stoull(parts[i]));
                } catch (...) {
                    return "ERR invalid size: " + parts[i];
                }
            }
            if (sizes.empty()) {
                sizes.push_back(4 * 1024 * 1024); // 4 MiB default
            }

            auto r = engine_.create_session(pid1, pid2, sizes);
            if (r) return "OK " + r.value();
            return "ERR " + r.err();
        }

        if (verb == "DESTROY") {
            if (parts.size() < 2)
                return "ERR usage: DESTROY <session_id>";
            auto r = engine_.destroy_session(parts[1]);
            if (r) return "OK destroyed";
            return "ERR " + r.err();
        }

        if (verb == "LIST") {
            auto sessions = engine_.list_sessions();
            std::ostringstream ss;
            ss << "OK " << sessions.size() << " sessions\n";
            for (const auto& s : sessions) {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - s.metrics.started_at);
                ss << s.session_id << " "
                   << s.pid1 << "<->" << s.pid2 << " "
                   << s.region_ids.size() << " regions "
                   << s.total_shared_bytes << " bytes "
                   << elapsed.count() << "s uptime\n";
            }
            return ss.str();
        }

        if (verb == "STATS") {
            auto stats = engine_.get_stats();
            std::ostringstream ss;
            ss << "OK\n"
               << "active_sessions:  " << stats.active_sessions << "\n"
               << "total_regions:    " << stats.total_regions << "\n"
               << "total_shared_mb:  "
               << (stats.total_shared_bytes / (1024.0 * 1024.0)) << "\n"
               << "total_messages:   " << stats.total_messages << "\n"
               << "monitor_checks:   " << monitor_.check_count();
            return ss.str();
        }

        if (verb == "GET") {
            if (parts.size() < 2)
                return "ERR usage: GET <session_id>";
            auto r = engine_.get_session(parts[1]);
            if (!r) return "ERR " + r.err();
            const auto& s = r.value();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - s.metrics.started_at);
            std::ostringstream ss;
            ss << "OK\n"
               << "session_id:      " << s.session_id << "\n"
               << "pid1:            " << s.pid1 << "\n"
               << "pid2:            " << s.pid2 << "\n"
               << "regions:         " << s.region_ids.size() << "\n"
               << "total_bytes:     " << s.total_shared_bytes << "\n"
               << "active:          " << (s.active ? "yes" : "no") << "\n"
               << "uptime_sec:      " << elapsed.count() << "\n"
               << "messages:        " << s.metrics.messages_exchanged << "\n"
               << "bytes_xferred:   " << s.metrics.bytes_transferred;
            return ss.str();
        }

        return "ERR unknown command: " + verb;
    }
};

int main(int /*argc*/, char* /*argv*/[]) {
    FuseDaemon daemon;
    return daemon.run();
}
