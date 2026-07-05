// services/nerve/main.cpp
// straylight-nerve — hardware interrupt affinity daemon.
// Routes interrupts to optimal CPU cores using fabric topology.

#include "affinity_optimizer.h"
#include "irq_mapper.h"
#include "irq_monitor.h"

#include <straylight/daemon.h>
#include <straylight/config.h>
#include <straylight/log.h>
#include <straylight/ipc_client.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>

namespace straylight {

class FabricPlacementCache {
public:
    FabricPlacementCache() {
        snapshot_["schema_version"] = "straylight.fabric.placement.v1";
        snapshot_["owner"] = "nerve";
        snapshot_["source"] = "fabric";
        snapshot_["policy"] = "observe_only";
        snapshot_["connected"] = false;
        snapshot_["locality_islands"] = nlohmann::json::array();
    }

    Result<void, std::string> refresh(const std::string& socket_path) {
        auto result = request_placement(socket_path);
        std::lock_guard lock(mu_);

        last_refresh_epoch_ms_ = now_epoch_ms();
        last_error_.clear();
        socket_path_ = socket_path;

        if (!result.has_value()) {
            last_error_ = result.error();
            snapshot_["connected"] = false;
            snapshot_["last_error"] = last_error_;
            snapshot_["last_refresh_epoch_ms"] = last_refresh_epoch_ms_;
            return Result<void, std::string>::error(last_error_);
        }

        auto payload = result.value();
        snapshot_ = payload;
        snapshot_["owner"] = "nerve";
        snapshot_["source"] = "fabric";
        snapshot_["policy"] = payload.value("policy", "observe_only");
        snapshot_["connected"] = true;
        snapshot_["socket_path"] = socket_path_;
        snapshot_["last_refresh_epoch_ms"] = last_refresh_epoch_ms_;
        snapshot_.erase("last_error");
        return Result<void, std::string>::ok();
    }

    nlohmann::json to_json() const {
        std::lock_guard lock(mu_);
        return snapshot_;
    }

    size_t island_count() const {
        std::lock_guard lock(mu_);
        if (!snapshot_.contains("locality_islands") ||
            !snapshot_["locality_islands"].is_array()) {
            return 0;
        }
        return snapshot_["locality_islands"].size();
    }

    bool connected() const {
        std::lock_guard lock(mu_);
        return snapshot_.value("connected", false);
    }

    std::string last_error() const {
        std::lock_guard lock(mu_);
        return last_error_;
    }

private:
    static int64_t now_epoch_ms() {
        auto now = std::chrono::system_clock::now().time_since_epoch();
        return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    }

    static bool write_all(int fd, const void* data, size_t len) {
        const char* ptr = static_cast<const char*>(data);
        size_t total = 0;
        while (total < len) {
            ssize_t n = ::send(fd, ptr + total, len - total, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            total += static_cast<size_t>(n);
        }
        return true;
    }

    static bool read_exact(int fd, void* data, size_t len) {
        char* ptr = static_cast<char*>(data);
        size_t total = 0;
        while (total < len) {
            ssize_t n = ::recv(fd, ptr + total, len - total, 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            total += static_cast<size_t>(n);
        }
        return true;
    }

    static Result<nlohmann::json, std::string> recv_frame(int fd) {
        uint32_t len = 0;
        if (!read_exact(fd, &len, sizeof(len))) {
            return Result<nlohmann::json, std::string>::error("fabric frame header read failed");
        }
        if (len == 0 || len > 4 * 1024 * 1024) {
            return Result<nlohmann::json, std::string>::error("fabric frame length invalid");
        }

        std::string body(len, '\0');
        if (!read_exact(fd, body.data(), body.size())) {
            return Result<nlohmann::json, std::string>::error("fabric frame body read failed");
        }

        try {
            return Result<nlohmann::json, std::string>::ok(nlohmann::json::parse(body));
        } catch (const std::exception& e) {
            return Result<nlohmann::json, std::string>::error(
                std::string("fabric JSON parse failed: ") + e.what());
        }
    }

    static Result<nlohmann::json, std::string> request_placement(
        const std::string& socket_path) {
        int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            return Result<nlohmann::json, std::string>::error(
                std::string("socket() failed: ") + ::strerror(errno));
        }

        struct timeval timeout{};
        timeout.tv_sec = 2;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            int e = errno;
            ::close(fd);
            return Result<nlohmann::json, std::string>::error(
                std::string("connect() to fabric failed: ") + ::strerror(e));
        }

        nlohmann::json request;
        request["id"] = "nerve-placement";
        request["method"] = "placement_planes";
        std::string payload = request.dump();
        uint32_t len = static_cast<uint32_t>(payload.size());

        if (!write_all(fd, &len, sizeof(len)) ||
            !write_all(fd, payload.data(), payload.size())) {
            ::close(fd);
            return Result<nlohmann::json, std::string>::error("fabric request write failed");
        }

        for (int i = 0; i < 8; ++i) {
            auto frame = recv_frame(fd);
            if (!frame.has_value()) {
                ::close(fd);
                return frame;
            }

            auto msg = frame.value();
            if (msg.value("type", "") == "res" &&
                msg.value("id", "") == "nerve-placement") {
                ::close(fd);
                if (!msg.contains("payload")) {
                    return Result<nlohmann::json, std::string>::error(
                        "fabric response missing payload");
                }
                return Result<nlohmann::json, std::string>::ok(msg["payload"]);
            }
        }

        ::close(fd);
        return Result<nlohmann::json, std::string>::error(
            "fabric response not received before event limit");
    }

    mutable std::mutex mu_;
    nlohmann::json snapshot_;
    std::string socket_path_;
    std::string last_error_;
    int64_t last_refresh_epoch_ms_ = 0;
};

/// IPC server for nerve daemon — handles JSON-RPC requests.
class NerveIpcServer {
public:
    NerveIpcServer() = default;
    ~NerveIpcServer() { stop(); }

    void set_monitor(IrqMonitor* monitor) { monitor_ = monitor; }
    void set_placement_cache(FabricPlacementCache* placement) { placement_ = placement; }

    Result<void, std::string> start(const std::string& socket_path, int max_clients) {
        socket_path_ = socket_path;
        ::unlink(socket_path.c_str());

        listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (listen_fd_ < 0) {
            return Result<void, std::string>::error(
                std::string("socket() failed: ") + ::strerror(errno));
        }

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

        if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            int e = errno;
            ::close(listen_fd_);
            listen_fd_ = -1;
            return Result<void, std::string>::error(
                std::string("bind() failed: ") + ::strerror(e));
        }

        if (::listen(listen_fd_, max_clients) < 0) {
            int e = errno;
            ::close(listen_fd_);
            listen_fd_ = -1;
            return Result<void, std::string>::error(
                std::string("listen() failed: ") + ::strerror(e));
        }

        int flags = ::fcntl(listen_fd_, F_GETFL, 0);
        if (flags >= 0) ::fcntl(listen_fd_, F_SETFL, flags | O_NONBLOCK);

        running_.store(true);
        accept_thread_ = std::thread([this]() { accept_loop(); });
        return Result<void, std::string>::ok();
    }

    void stop() {
        running_.store(false);
        if (listen_fd_ >= 0) { ::close(listen_fd_); listen_fd_ = -1; }
        if (accept_thread_.joinable()) accept_thread_.join();
        std::lock_guard lock(clients_mu_);
        for (int fd : client_fds_) ::close(fd);
        client_fds_.clear();
        if (!socket_path_.empty()) ::unlink(socket_path_.c_str());
    }

private:
    void accept_loop() {
        while (running_.load()) {
            struct pollfd pfd{};
            pfd.fd = listen_fd_;
            pfd.events = POLLIN;
            if (::poll(&pfd, 1, 500) <= 0) continue;

            int client_fd = ::accept(listen_fd_, nullptr, nullptr);
            if (client_fd < 0) continue;

            {
                std::lock_guard lock(clients_mu_);
                client_fds_.push_back(client_fd);
            }

            std::thread([this, client_fd]() { handle_client(client_fd); }).detach();
        }
    }

    void handle_client(int fd) {
        char buf[65536];
        std::string accumulated;

        while (running_.load()) {
            struct pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLIN;
            if (::poll(&pfd, 1, 1000) <= 0) continue;

            ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n <= 0) break;

            accumulated.append(buf, static_cast<size_t>(n));

            size_t pos;
            while ((pos = accumulated.find('\n')) != std::string::npos) {
                std::string message = accumulated.substr(0, pos);
                accumulated.erase(0, pos + 1);
                if (message.empty()) continue;

                nlohmann::json response;
                try {
                    auto request = nlohmann::json::parse(message);
                    response = handle_request(request);
                } catch (const nlohmann::json::parse_error& e) {
                    response["jsonrpc"] = "2.0";
                    response["error"]["code"] = -32700;
                    response["error"]["message"] = std::string("Parse error: ") + e.what();
                }

                std::string out = response.dump() + "\n";
                size_t total = 0;
                while (total < out.size()) {
                    ssize_t w = ::write(fd, out.data() + total, out.size() - total);
                    if (w <= 0) goto done;
                    total += static_cast<size_t>(w);
                }
            }
        }

    done:
        ::close(fd);
        std::lock_guard lock(clients_mu_);
        client_fds_.erase(
            std::remove(client_fds_.begin(), client_fds_.end(), fd),
            client_fds_.end());
    }

    nlohmann::json handle_request(const nlohmann::json& req) {
        nlohmann::json resp;
        resp["jsonrpc"] = "2.0";
        if (req.contains("id")) resp["id"] = req["id"];

        std::string method = req.value("method", "");
        auto params = req.value("params", nlohmann::json::object());

        if (method == "status") {
            auto irqs = IrqMapper::scan_irqs();
            if (irqs.has_value()) {
                nlohmann::json arr = nlohmann::json::array();
                for (const auto& irq : irqs.value()) {
                    nlohmann::json entry;
                    entry["irq"] = irq.irq_number;
                    entry["device"] = irq.device_name;
                    entry["type"] = irq.type == IrqType::MSIX ? "MSI-X" :
                                   irq.type == IrqType::MSI ? "MSI" : "Legacy";
                    entry["cpus"] = irq.current_cpu_affinity;
                    entry["total_count"] = irq.total_count;
                    arr.push_back(entry);
                }
                resp["result"] = arr;
            } else {
                resp["error"]["code"] = -32000;
                resp["error"]["message"] = irqs.error();
            }

        } else if (method == "optimize") {
            auto result = AffinityOptimizer::optimize();
            if (result.has_value()) {
                nlohmann::json arr = nlohmann::json::array();
                for (const auto& rec : result.value()) {
                    nlohmann::json entry;
                    entry["irq"] = rec.irq_number;
                    entry["device"] = rec.device_name;
                    entry["previous_cpus"] = rec.previous_cpus;
                    entry["new_cpus"] = rec.recommended_cpus;
                    entry["reason"] = rec.reason;
                    arr.push_back(entry);
                }
                resp["result"] = {{"changes", arr}, {"count", arr.size()}};
            } else {
                resp["error"]["code"] = -32000;
                resp["error"]["message"] = result.error();
            }

        } else if (method == "set_affinity") {
            uint32_t irq = params.value("irq", 0u);
            std::string mask = params.value("mask", "");
            if (mask.empty()) {
                resp["error"]["code"] = -32602;
                resp["error"]["message"] = "Missing 'mask' parameter";
            } else {
                auto r = IrqMapper::set_affinity(irq, mask);
                if (r.has_value()) {
                    resp["result"] = {{"status", "ok"}, {"irq", irq}, {"mask", mask}};
                } else {
                    resp["error"]["code"] = -32000;
                    resp["error"]["message"] = r.error();
                }
            }

        } else if (method == "monitor") {
            auto rates = monitor_->get_rates_sorted();
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& [irq, rate] : rates) {
                nlohmann::json entry;
                entry["irq"] = irq;
                entry["rate"] = rate;
                arr.push_back(entry);
            }
            resp["result"] = arr;

        } else if (method == "balance_report") {
            resp["result"] = {{"report", AffinityOptimizer::balance_report()}};

        } else if (method == "placement_report") {
            if (placement_ != nullptr) {
                resp["result"] = placement_->to_json();
            } else {
                resp["error"]["code"] = -32000;
                resp["error"]["message"] = "Fabric placement cache is unavailable";
            }

        } else if (method == "alerts") {
            auto alerts = monitor_->get_alerts();
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& alert : alerts) {
                nlohmann::json entry;
                entry["irq"] = alert.irq_number;
                entry["device"] = alert.device_name;
                entry["message"] = alert.message;
                entry["severity"] = alert.severity == IrqAlert::Severity::Critical ? "critical" :
                                   alert.severity == IrqAlert::Severity::Warning ? "warning" : "info";
                arr.push_back(entry);
            }
            resp["result"] = arr;

        } else {
            resp["error"]["code"] = -32601;
            resp["error"]["message"] = "Unknown method: " + method;
        }

        return resp;
    }

    IrqMonitor* monitor_ = nullptr;
    FabricPlacementCache* placement_ = nullptr;
    int listen_fd_ = -1;
    std::string socket_path_;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    std::mutex clients_mu_;
    std::vector<int> client_fds_;
};

/// Watches for topology changes (CPU online/offline, device hotplug).
class TopologyWatcher {
public:
    using Callback = std::function<void()>;

    TopologyWatcher() = default;
    ~TopologyWatcher() { stop(); }

    void start(Callback on_change) {
        callback_ = std::move(on_change);
        running_.store(true);
        thread_ = std::thread([this]() { watch_loop(); });
    }

    void stop() {
        running_.store(false);
        if (inotify_fd_ >= 0) { ::close(inotify_fd_); inotify_fd_ = -1; }
        if (thread_.joinable()) thread_.join();
    }

private:
    void watch_loop() {
        inotify_fd_ = ::inotify_init1(IN_NONBLOCK);
        if (inotify_fd_ < 0) return;

        // Watch CPU online/offline
        ::inotify_add_watch(inotify_fd_, "/sys/devices/system/cpu",
                           IN_CREATE | IN_DELETE);

        // Watch PCI hotplug
        ::inotify_add_watch(inotify_fd_, "/sys/bus/pci/devices",
                           IN_CREATE | IN_DELETE);

        while (running_.load()) {
            struct pollfd pfd{};
            pfd.fd = inotify_fd_;
            pfd.events = POLLIN;

            if (::poll(&pfd, 1, 2000) > 0) {
                char buf[4096];
                ssize_t n = ::read(inotify_fd_, buf, sizeof(buf));
                if (n > 0 && callback_) {
                    callback_();
                }
            }
        }
    }

    Callback callback_;
    int inotify_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

/// Nerve daemon — optimizes IRQ affinity and monitors interrupt health.
class NerveDaemon : public DaemonBase {
public:
    Result<void, SLError> init(const Config& cfg) override {
        SL_INFO("nerve: initializing daemon");

        tick_interval_ms_ = cfg.get<int>("daemon.tick_interval_ms", 1000);
        monitor_interval_ms_ = cfg.get<int>("monitor.interval_ms", 1000);
        auto_optimize_ = cfg.get<bool>("optimizer.auto", true);
        auto_optimize_interval_s_ = cfg.get<int>("optimizer.interval_s", 300);
        fabric_enabled_ = cfg.get<bool>("fabric.enabled", true);
        fabric_socket_path_ = cfg.get<std::string>(
            "fabric.socket_path", "/run/straylight/fabric.sock");
        fabric_refresh_interval_s_ = cfg.get<int>("fabric.refresh_interval_s", 30);

        uint64_t storm_thresh = cfg.get<uint64_t>("monitor.storm_threshold", 100000);
        monitor_.set_storm_threshold(storm_thresh);

        double imbalance_thresh = cfg.get<double>("monitor.imbalance_threshold", 10.0);
        monitor_.set_imbalance_threshold(imbalance_thresh);

        // Start IPC server
        std::string socket_path = cfg.get<std::string>(
            "ipc.socket_path", "/run/straylight/nerve.sock");
        int max_clients = cfg.get<int>("ipc.max_clients", 8);

        ipc_.set_monitor(&monitor_);
        ipc_.set_placement_cache(&fabric_);
        auto ipc_result = ipc_.start(socket_path, max_clients);
        if (!ipc_result.has_value()) {
            SL_WARN("nerve: IPC server failed to start: {}", ipc_result.error());
        }

        refresh_fabric("initial");

        // Start topology watcher
        topo_watcher_.start([this]() {
            SL_INFO("nerve: topology change detected, refreshing placement");
            needs_fabric_refresh_.store(true);
            needs_reoptimize_.store(true);
        });

        // Initial optimization
        if (auto_optimize_) {
            SL_INFO("nerve: running initial IRQ optimization");
            auto result = AffinityOptimizer::optimize();
            if (result.has_value()) {
                SL_INFO("nerve: applied {} IRQ affinity changes", result.value().size());
            } else {
                SL_WARN("nerve: initial optimization failed: {}", result.error());
            }
        }

        SL_INFO("nerve: daemon initialized (tick={}ms, monitor={}ms)",
                tick_interval_ms_, monitor_interval_ms_);
        return Result<void, SLError>::ok();
    }

    Result<void, SLError> tick() override {
        std::this_thread::sleep_for(std::chrono::milliseconds(tick_interval_ms_));

        // Sample interrupt rates
        auto sample_result = monitor_.sample();
        if (!sample_result.has_value()) {
            SL_WARN("nerve: monitor sample failed: {}", sample_result.error());
        }

        // Check for alerts
        auto alerts = monitor_.get_and_clear_alerts();
        for (const auto& alert : alerts) {
            if (alert.severity == IrqAlert::Severity::Critical) {
                SL_ERROR("nerve: ALERT: {}", alert.message);
            } else if (alert.severity == IrqAlert::Severity::Warning) {
                SL_WARN("nerve: ALERT: {}", alert.message);
            } else {
                SL_INFO("nerve: {}", alert.message);
            }
        }

        // Check if storm detected — trigger thermal rebalance for affected cores
        if (monitor_.is_storm_detected()) {
            SL_WARN("nerve: IRQ storm detected, checking thermal status");
        }

        maybe_refresh_fabric();

        // Periodic re-optimization
        if (auto_optimize_) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now - last_optimize_time_);

            if (elapsed.count() >= auto_optimize_interval_s_ || needs_reoptimize_.load()) {
                needs_reoptimize_.store(false);
                last_optimize_time_ = now;

                auto result = AffinityOptimizer::optimize();
                if (result.has_value() && !result.value().empty()) {
                    SL_INFO("nerve: rebalanced {} IRQs", result.value().size());
                }
            }
        }

        return Result<void, SLError>::ok();
    }

    void shutdown() override {
        SL_INFO("nerve: shutting down");
        topo_watcher_.stop();
        ipc_.stop();
        SL_INFO("nerve: shutdown complete");
    }

private:
    void refresh_fabric(const char* reason) {
        if (!fabric_enabled_) return;
        auto result = fabric_.refresh(fabric_socket_path_);
        if (result.has_value()) {
            SL_INFO("nerve: Fabric placement refresh ({}) saw {} island(s)",
                    reason, fabric_.island_count());
        } else {
            SL_WARN("nerve: Fabric placement refresh ({}) failed: {}",
                    reason, result.error());
        }
        last_fabric_refresh_time_ = std::chrono::steady_clock::now();
        needs_fabric_refresh_.store(false);
    }

    void maybe_refresh_fabric() {
        if (!fabric_enabled_) return;
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_fabric_refresh_time_);
        if (needs_fabric_refresh_.load() ||
            elapsed.count() >= fabric_refresh_interval_s_) {
            refresh_fabric("periodic");
        }
    }

    IrqMonitor monitor_;
    FabricPlacementCache fabric_;
    NerveIpcServer ipc_;
    TopologyWatcher topo_watcher_;

    int tick_interval_ms_ = 1000;
    int monitor_interval_ms_ = 1000;
    bool auto_optimize_ = true;
    int auto_optimize_interval_s_ = 300;
    bool fabric_enabled_ = true;
    std::string fabric_socket_path_ = "/run/straylight/fabric.sock";
    int fabric_refresh_interval_s_ = 30;

    std::atomic<bool> needs_reoptimize_{false};
    std::atomic<bool> needs_fabric_refresh_{false};
    std::chrono::steady_clock::time_point last_optimize_time_ =
        std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point last_fabric_refresh_time_ =
        std::chrono::steady_clock::now() - std::chrono::seconds(3600);
};

} // namespace straylight

int main(int /*argc*/, char* /*argv*/[]) {
    straylight::Log::init("straylight-nerve");

    auto cfg_result = straylight::Config::load("/etc/straylight/nerve.conf");
    if (!cfg_result.has_value()) {
        SL_ERROR("nerve: failed to load config: {}", cfg_result.error());
        return 1;
    }

    straylight::NerveDaemon daemon;
    return daemon.run(cfg_result.value());
}
