/**
 * StrayLight Quota Daemon — Per-App Resource Budget Enforcement
 *
 * Listens on a Unix domain socket for quota commands:
 *   SET <app> <pid> cpu=N ram=N vram=N gpu=N disk_iops=N net=N fps=N
 *   GET <app>                Get quota + usage for an app
 *   LIST                     List all tracked apps
 *   USAGE                    Full usage report
 *   ENFORCE <on|off>         Enable/disable enforcement
 *   VIOLATIONS [count]       Recent violations
 *
 * Periodically reads cgroup/sysfs/compositor and enforces limits.
 */

#include "cgroup_controller.h"
#include "quota_config.h"
#include "quota_engine.h"

#include <straylight/config.h>
#include <straylight/daemon.h>
#include <straylight/log.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

static constexpr const char* SOCKET_PATH =
    "/var/run/straylight/quota.sock";

namespace {

std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> parts;
    std::istringstream iss(s);
    std::string token;
    while (iss >> token) parts.push_back(token);
    return parts;
}

/** Parse "key=value" pairs into a ResourceQuota, modifying it in place. */
void parse_quota_args(straylight::quota::ResourceQuota& q,
                       const std::vector<std::string>& args,
                       size_t start_idx) {
    for (size_t i = start_idx; i < args.size(); ++i) {
        auto eq = args[i].find('=');
        if (eq == std::string::npos) continue;
        std::string key = args[i].substr(0, eq);
        std::string val = args[i].substr(eq + 1);
        try {
            if (key == "cpu")        q.cpu_percent = std::stod(val);
            else if (key == "ram")   q.ram_bytes = std::stoull(val);
            else if (key == "vram")  q.vram_bytes = std::stoull(val);
            else if (key == "gpu")   q.gpu_compute_percent = std::stod(val);
            else if (key == "disk_iops") q.disk_iops = std::stoull(val);
            else if (key == "net")   q.net_bandwidth = std::stoull(val);
            else if (key == "fps")   q.compositor_fps = std::stod(val);
        } catch (...) {}
    }
}

} // anonymous namespace

class QuotaDaemon : public straylight::DaemonBase {
public:
    QuotaDaemon()
        : config_()
        , cgroup_()
        , engine_(config_, cgroup_)
    {}

    straylight::Result<void, straylight::SLError> init(
            const straylight::Config& /*cfg*/) override {
        // Load config
        config_.load();

        // Pre-create the straylight cgroup root
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(straylight::quota::CGROUP_ROOT, ec);

        // Create Unix socket
        listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
        if (listen_fd_ < 0) {
            return fail(std::string("socket(): ") + strerror(errno));
        }

        fs::remove(SOCKET_PATH, ec);
        fs::create_directories(fs::path(SOCKET_PATH).parent_path(), ec);

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

        if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr),
                 sizeof(addr)) < 0) {
            close(listen_fd_);
            return fail(std::string("bind(): ") + strerror(errno));
        }

        chmod(SOCKET_PATH, 0666);

        if (listen(listen_fd_, 8) < 0) {
            close(listen_fd_);
            return fail(std::string("listen(): ") + strerror(errno));
        }

        int flags = fcntl(listen_fd_, F_GETFL, 0);
        fcntl(listen_fd_, F_SETFL, flags | O_NONBLOCK);

        SL_INFO("quota: listening on {}", SOCKET_PATH);
        return straylight::Result<void, straylight::SLError>::ok();
    }

    straylight::Result<void, straylight::SLError> tick() override {
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

        // Run enforcement every tick
        engine_.enforce();
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        return straylight::Result<void, straylight::SLError>::ok();
    }

    void shutdown() override {
        if (listen_fd_ >= 0) {
            close(listen_fd_);
            listen_fd_ = -1;
        }
        std::error_code ec;
        std::filesystem::remove(SOCKET_PATH, ec);

        // Save config on clean shutdown
        config_.save();
    }

private:
    straylight::quota::QuotaConfig config_;
    straylight::quota::CgroupController cgroup_;
    straylight::quota::QuotaEngine engine_;
    int listen_fd_ = -1;

    static straylight::Result<void, straylight::SLError> fail(
            const std::string& message) {
        return straylight::Result<void, straylight::SLError>::error(
            straylight::SLError{straylight::SLErrorCode::IpcFailed, message});
    }

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

        if (verb == "SET") {
            if (parts.size() < 3)
                return "ERR usage: SET <app> <pid> [key=value ...]";

            std::string app = parts[1];
            pid_t pid = 0;
            try { pid = static_cast<pid_t>(std::stoi(parts[2])); }
            catch (...) { return "ERR invalid PID"; }

            // Start from existing or default quota
            auto quota = config_.get_quota(app);
            parse_quota_args(quota, parts, 3);

            auto r = engine_.set_quota(app, pid, quota);
            if (r) return "OK quota set for " + app;
            return "ERR " + r.error();
        }

        if (verb == "GET") {
            if (parts.size() < 2)
                return "ERR usage: GET <app>";

            auto quota = config_.get_quota(parts[1]);
            auto usage_r = engine_.get_usage(parts[1]);

            std::ostringstream ss;
            ss << "OK\n"
               << "app:              " << parts[1] << "\n"
               << "--- quota ---\n"
               << "cpu_percent:      " << quota.cpu_percent << "\n"
               << "ram_bytes:        " << quota.ram_bytes << "\n"
               << "vram_bytes:       " << quota.vram_bytes << "\n"
               << "gpu_compute_pct:  " << quota.gpu_compute_percent << "\n"
               << "disk_iops:        " << quota.disk_iops << "\n"
               << "net_bandwidth:    " << quota.net_bandwidth << "\n"
               << "compositor_fps:   " << quota.compositor_fps;

            if (usage_r) {
                const auto& u = usage_r.value();
                ss << "\n--- usage ---\n"
                   << "cpu_percent:      " << u.cpu_percent << "\n"
                   << "ram_bytes:        " << u.ram_bytes << "\n"
                   << "vram_bytes:       " << u.vram_bytes << "\n"
                   << "gpu_compute_pct:  " << u.gpu_compute_percent << "\n"
                   << "disk_iops:        " << u.disk_iops << "\n"
                   << "net_bandwidth:    " << u.net_bandwidth << "\n"
                   << "compositor_fps:   " << u.compositor_fps << "\n"
                   << "last_action:      "
                   << straylight::quota::action_str(u.last_action);
            }
            return ss.str();
        }

        if (verb == "LIST") {
            auto apps = config_.list_apps();
            std::ostringstream ss;
            ss << "OK " << apps.size() << " apps\n";
            for (const auto& app : apps) {
                auto q = config_.get_quota(app);
                ss << app
                   << " cpu=" << q.cpu_percent
                   << " ram=" << q.ram_bytes
                   << " vram=" << q.vram_bytes
                   << " gpu=" << q.gpu_compute_percent
                   << "\n";
            }
            return ss.str();
        }

        if (verb == "USAGE") {
            auto all = engine_.list_all_usage();
            std::ostringstream ss;
            ss << "OK " << all.size() << " apps\n";
            for (const auto& u : all) {
                ss << u.app_name
                   << " pid=" << u.pid
                   << " cpu=" << u.cpu_percent << "%"
                   << " ram=" << (u.ram_bytes / (1024*1024)) << "M"
                   << " vram=" << (u.vram_bytes / (1024*1024)) << "M"
                   << " gpu=" << u.gpu_compute_percent << "%"
                   << " iops=" << u.disk_iops
                   << " action=" << straylight::quota::action_str(u.last_action)
                   << "\n";
            }
            return ss.str();
        }

        if (verb == "ENFORCE") {
            if (parts.size() < 2)
                return "ERR usage: ENFORCE <on|off>";
            bool enabled = (parts[1] == "on" || parts[1] == "1" ||
                            parts[1] == "true");
            engine_.set_enforcement_enabled(enabled);
            return std::string("OK enforcement ") +
                   (enabled ? "enabled" : "disabled");
        }

        if (verb == "VIOLATIONS") {
            size_t count = 50;
            if (parts.size() > 1) {
                try { count = std::stoull(parts[1]); } catch (...) {}
            }
            auto violations = engine_.get_violations(count);
            std::ostringstream ss;
            ss << "OK " << violations.size() << " violations\n";
            for (const auto& v : violations) {
                ss << v.timestamp << " " << v.app_name << " " << v.resource
                   << " " << v.usage_percent << "% "
                   << straylight::quota::action_str(v.action) << "\n";
            }
            return ss.str();
        }

        return "ERR unknown command: " + verb;
    }
};

int main(int /*argc*/, char* /*argv*/[]) {
    straylight::Log::init("straylight-quota");
    QuotaDaemon daemon;
    return daemon.run(straylight::Config::make_empty());
}
