/**
 * StrayLight Fabric — Daemon
 *
 * Unified device topology graph daemon. Scans the system hardware,
 * builds a graph of all devices with bandwidth/latency edges,
 * monitors udev for hotplug events, and serves topology queries.
 *
 * Usage:
 *   straylight-fabric                    # run as daemon
 *   straylight-fabric --dump-json        # dump topology and exit
 */

#include "query_engine.h"
#include "topology.h"
#include "udev_monitor.h"
#include "straylight/daemon.h"
#include "straylight/result.h"
#include "straylight/error.h"
#include "straylight/config.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <grp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static bool fabric_set_nonblocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

namespace straylight::fabric {

// ── Daemon ──────────────────────────────────────────────────────────

class FabricDaemon : public DaemonBase {
public:
    FabricDaemon()
        : query_(topo_)
        , udev_(topo_)
    {}

protected:
    Result<void, SLError> init(const Config& /*cfg*/) override {
        fprintf(stdout, "[fabric] scanning system topology...\n");

        auto r = topo_.build_topology();
        if (!r) return Result<void, SLError>::error(SLError{SLErrorCode::Internal, r.error()});

        fprintf(stdout, "[fabric] found %zu devices, %zu links\n",
                topo_.node_count(), topo_.edge_count());

        // Save initial topology to JSON
        save_topology_json();

        // Start udev monitor
        auto udev_res = udev_.start();
        if (udev_res) {
            fprintf(stdout, "[fabric] udev monitor started\n");
        } else {
            fprintf(stderr, "[fabric] udev monitor failed: %s (non-fatal)\n",
                    udev_res.error().c_str());
        }

        // Register for topology change notifications
        udev_.on_change([this](const UdevEvent& event) {
            fprintf(stdout, "[fabric] topology changed: %s %s\n",
                    udev_action_str(event.action).c_str(),
                    event.devpath.c_str());
            save_topology_json();
        });

        // Set up command interface
        setup_command_socket();
        return Result<void, SLError>::ok();
    }

    Result<void, SLError> tick() override {
        poll_ipc(50);
        process_commands();
        tick_count_++;
        if (tick_count_ % 60 == 0) save_topology_json();
        return Result<void, SLError>::ok();
    }

    void shutdown() override {
        teardown_ipc();
        udev_.stop();
        save_topology_json();
        cleanup_command_files();
        fprintf(stdout, "[fabric] udev events processed: %lu\n",
                static_cast<unsigned long>(udev_.events_processed()));
    }

    void on_reload() {
        fprintf(stdout, "[fabric] rebuilding topology...\n");
        topo_.build_topology();
        save_topology_json();
        fprintf(stdout, "[fabric] topology rebuilt: %zu devices, %zu links\n",
                topo_.node_count(), topo_.edge_count());
    }

private:
    Topology topo_;
    QueryEngine query_;
    UdevMonitor udev_;
    uint64_t tick_count_ = 0;
    std::string socket_path_ = "/run/straylight/fabric.sock";
    std::string json_path_ = "/var/lib/straylight/fabric/topology.json";

    // ── IPC epoll server ──────────────────────────────────────────────
    int server_fd_ = -1;
    int epoll_fd_  = -1;
    std::vector<int> client_fds_;

    void save_topology_json() {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(fs::path(json_path_).parent_path(), ec);
        std::ofstream out(json_path_, std::ios::trunc);
        if (out) out << topo_.to_json();
    }

    // Real Unix socket IPC server
    void setup_ipc_socket() {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(fs::path(socket_path_).parent_path(), ec);

        server_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (server_fd_ < 0) { perror("[fabric] socket"); return; }
        fabric_set_nonblocking(server_fd_);

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        ::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);
        ::unlink(socket_path_.c_str());

        if (::bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            perror("[fabric] bind"); ::close(server_fd_); server_fd_ = -1; return;
        }
        ::listen(server_fd_, 8);

        if (auto* group = ::getgrnam("straylight")) {
            ::chown(socket_path_.c_str(), 0, group->gr_gid);
        }
        ::chmod(socket_path_.c_str(), 0660);

        epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd_ < 0) {
            perror("[fabric] epoll_create"); ::close(server_fd_); server_fd_ = -1; return;
        }
        epoll_event ev{};
        ev.events  = EPOLLIN;
        ev.data.fd = server_fd_;
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, server_fd_, &ev);
        fprintf(stdout, "[fabric] IPC listening on %s\n", socket_path_.c_str());
    }

    void teardown_ipc() {
        for (int fd : client_fds_) ::close(fd);
        client_fds_.clear();
        if (epoll_fd_ >= 0) { ::close(epoll_fd_); epoll_fd_ = -1; }
        if (server_fd_ >= 0) { ::close(server_fd_); server_fd_ = -1; }
        ::unlink(socket_path_.c_str());
    }

    void poll_ipc(int timeout_ms) {
        if (epoll_fd_ < 0) return;
        epoll_event events[16];
        int n = ::epoll_wait(epoll_fd_, events, 16, timeout_ms);
        for (int i = 0; i < n; ++i) {
            if (events[i].data.fd == server_fd_) accept_client();
            else handle_ipc_client(events[i].data.fd);
        }
        if (!client_fds_.empty()) {
            nlohmann::json ev;
            ev["service"] = "fabric";
            ev["type"]    = "event";
            ev["payload"]["device_count"] = topo_.node_count();
            ev["payload"]["link_count"]   = topo_.edge_count();
            ev["payload"]["locality_island_count"] = topo_.locality_island_count();
            std::string s = ev.dump();
            std::vector<int> dead;
            for (int fd : client_fds_)
                if (!send_frame(fd, s)) dead.push_back(fd);
            for (int fd : dead) remove_ipc_client(fd);
        }
    }

    void accept_client() {
        sockaddr_un addr{}; socklen_t len = sizeof(addr);
        int fd = ::accept4(server_fd_, reinterpret_cast<sockaddr*>(&addr), &len,
                           SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) return;
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLHUP | EPOLLERR;
        ev.data.fd = fd;
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev);
        client_fds_.push_back(fd);
    }

    void handle_ipc_client(int fd) {
        auto frame = recv_frame(fd);
        if (!frame) { remove_ipc_client(fd); return; }
        try {
            auto msg = nlohmann::json::parse(*frame);
            auto rep = dispatch_ipc(msg);
            send_frame(fd, rep.dump());
        } catch (...) {
            nlohmann::json err;
            err["service"] = "fabric"; err["type"] = "error";
            err["payload"]["message"] = "parse error";
            send_frame(fd, err.dump());
        }
    }

    void remove_ipc_client(int fd) {
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
        ::close(fd);
        client_fds_.erase(std::remove(client_fds_.begin(), client_fds_.end(), fd), client_fds_.end());
    }

    std::optional<std::string> recv_frame(int fd) {
        uint32_t len_le = 0;
        if (::recv(fd, &len_le, 4, MSG_WAITALL) != 4) return std::nullopt;
        uint32_t len = len_le;
        if (len == 0 || len > 4 * 1024 * 1024) return std::nullopt;
        std::string buf(len, '\0');
        if (::recv(fd, buf.data(), len, MSG_WAITALL) != static_cast<ssize_t>(len)) return std::nullopt;
        return buf;
    }

    bool send_frame(int fd, const std::string& data) {
        uint32_t len = static_cast<uint32_t>(data.size());
        if (::send(fd, &len, 4, MSG_NOSIGNAL) != 4) return false;
        return ::send(fd, data.data(), len, MSG_NOSIGNAL) == static_cast<ssize_t>(len);
    }

    nlohmann::json dispatch_ipc(const nlohmann::json& msg) {
        nlohmann::json r;
        r["service"] = "fabric"; r["type"] = "res";
        if (msg.contains("id")) r["id"] = msg["id"];
        std::string method = msg.value("method", "");
        if (method == "status" || method == "fabric_stats") {
            r["payload"]["device_count"] = topo_.node_count();
            r["payload"]["link_count"]   = topo_.edge_count();
            r["payload"]["locality_island_count"] = topo_.locality_island_count();
            r["payload"]["topology"]     = topo_.to_json();
        } else if (method == "locality" || method == "placement_planes") {
            r["payload"]["schema_version"] = "straylight.fabric.placement.v1";
            r["payload"]["owner"] = "fabric";
            r["payload"]["policy"] = "observe_only";
            r["payload"]["locality_islands"] =
                nlohmann::json::parse(topo_.locality_islands_to_json());
        } else if (method == "path") {
            std::string from = msg.value("from", ""), to = msg.value("to", "");
            auto res = topo_.find_path(from, to);
            if (res) {
                r["payload"]["hops"] = nlohmann::json::array();
                for (auto& h : res.value().hops) {
                    nlohmann::json hop;
                    hop["from"] = h.from; hop["to"] = h.to;
                    hop["bw_gbps"] = h.bandwidth_gbps;
                    hop["latency_ns"] = h.latency_ns;
                    r["payload"]["hops"].push_back(hop);
                }
            } else { r["type"] = "error"; r["payload"]["message"] = res.error(); }
        } else {
            r["type"] = "error";
            r["payload"]["message"] = "unknown method: " + method;
        }
        return r;
    }

    // Legacy file-based command interface (kept for backward compat)
    void setup_command_socket() { setup_ipc_socket(); }
    void cleanup_command_files() {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::remove(socket_path_ + ".cmd", ec);
        fs::remove(socket_path_ + ".resp", ec);
    }

    void process_commands() {
        auto cmd_path = socket_path_ + ".cmd";
        std::ifstream in(cmd_path);
        if (!in) return;

        std::string line;
        while (std::getline(in, line)) {
            handle_command(line);
        }
        in.close();
        std::ofstream clear(cmd_path, std::ios::trunc);
    }

    void handle_command(const std::string& line) {
        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;

        auto response_path = socket_path_ + ".resp";
        std::ofstream resp(response_path, std::ios::app);

        if (cmd == "topology") {
            resp << topo_.to_json();
        }
        else if (cmd == "path") {
            std::string from, to;
            iss >> from >> to;
            auto path_res = topo_.find_path(from, to);
            if (path_res) {
                auto& path = path_res.value();
                resp << "PATH: " << path.hops.size() << " hops, "
                     << path.total_latency_ns << " ns total latency, "
                     << path.bottleneck_bw_gbps << " Gbps bottleneck\n";
                for (auto& hop : path.hops) {
                    resp << "  " << hop.from << " -> " << hop.to
                         << " [" << link_type_str(hop.link_type)
                         << " " << hop.bandwidth_gbps << " Gbps, "
                         << hop.latency_ns << " ns]\n";
                }
            } else {
                resp << "ERROR: " << path_res.error() << "\n";
            }
        }
        else if (cmd == "fastest") {
            std::string from, to;
            iss >> from >> to;
            auto path_res = topo_.find_fastest_path(from, to);
            if (path_res) {
                auto& path = path_res.value();
                resp << "FASTEST: " << path.hops.size() << " hops, "
                     << path.bottleneck_bw_gbps << " Gbps bandwidth\n";
                for (auto& hop : path.hops) {
                    resp << "  " << hop.from << " -> " << hop.to
                         << " [" << link_type_str(hop.link_type)
                         << " " << hop.bandwidth_gbps << " Gbps]\n";
                }
            } else {
                resp << "ERROR: " << path_res.error() << "\n";
            }
        }
        else if (cmd == "bottleneck") {
            std::string from, to;
            iss >> from >> to;
            auto path_res = topo_.find_fastest_path(from, to);
            if (path_res) {
                auto bn_res = query_.get_bottleneck(path_res.value());
                if (bn_res) {
                    auto& bn = bn_res.value();
                    resp << "BOTTLENECK: " << bn.from << " -> " << bn.to
                         << " [" << link_type_str(bn.link_type)
                         << " " << bn.bandwidth_gbps << " Gbps, "
                         << bn.latency_ns << " ns] (hop " << bn.hop_index << ")\n";
                } else {
                    resp << "ERROR: " << bn_res.error() << "\n";
                }
            } else {
                resp << "ERROR: " << path_res.error() << "\n";
            }
        }
        else if (cmd == "affinity") {
            std::string device;
            iss >> device;
            auto aff_res = query_.get_affinity(device);
            if (aff_res) {
                auto& aff = aff_res.value();
                resp << "AFFINITY: device=" << aff.device_id
                     << " cpu=" << aff.closest_cpu
                     << " numa=" << aff.closest_numa
                     << " node=" << aff.numa_node
                     << " latency=" << aff.latency_ns << " ns\n";
            } else {
                resp << "ERROR: " << aff_res.error() << "\n";
            }
        }
        else if (cmd == "devices") {
            std::string type;
            iss >> type;
            std::vector<DeviceNode> devs;
            if (type.empty()) {
                devs = topo_.get_all_nodes();
            } else {
                devs = query_.get_devices(type);
            }
            resp << "DEVICES: " << devs.size() << "\n";
            for (auto& d : devs) {
                resp << "  " << d.id << " [" << device_type_str(d.type) << "] "
                     << d.name;
                if (d.bandwidth_gbps > 0) resp << " " << d.bandwidth_gbps << " Gbps";
                if (d.numa_node >= 0) resp << " numa=" << d.numa_node;
                resp << "\n";
            }
        }
        else if (cmd == "locality" || cmd == "placement") {
            resp << "LOCALITY: " << topo_.locality_island_count()
                 << " placement island(s)\n";
            resp << topo_.locality_islands_to_json() << "\n";
        }
        else if (cmd == "bandwidth") {
            std::string from, to;
            iss >> from >> to;
            auto bw_res = query_.get_bandwidth(from, to);
            if (bw_res) {
                resp << "BANDWIDTH: " << bw_res.value() << " Gbps\n";
            } else {
                resp << "ERROR: " << bw_res.error() << "\n";
            }
        }
        else if (cmd == "query") {
            std::string rest;
            std::getline(iss, rest);
            auto path_res = query_.query(rest);
            if (path_res) {
                auto& path = path_res.value();
                resp << "QUERY RESULT: " << path.hops.size() << " hops\n";
                for (auto& hop : path.hops) {
                    resp << "  " << hop.from << " -> " << hop.to
                         << " [" << hop.bandwidth_gbps << " Gbps, "
                         << hop.latency_ns << " ns]\n";
                }
            } else {
                resp << "ERROR: " << path_res.error() << "\n";
            }
        }
        else if (cmd == "transfer") {
            std::string from, to;
            uint64_t bytes;
            iss >> from >> to >> bytes;
            auto est_res = query_.estimate_transfer_time(from, to, bytes);
            if (est_res) {
                auto& est = est_res.value();
                resp << "TRANSFER: " << est.bytes << " bytes, "
                     << est.estimated_time_us << " us estimated, "
                     << est.bottleneck_bw_gbps << " Gbps bottleneck, "
                     << est.hop_count << " hops\n";
            } else {
                resp << "ERROR: " << est_res.error() << "\n";
            }
        }
        else {
            resp << "ERROR: unknown command: " << cmd << "\n";
        }
    }
};

} // namespace straylight::fabric

// ── main ────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    // Check for --dump-json mode
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dump-json") == 0) {
            straylight::fabric::Topology topo;
            topo.build_topology();
            printf("%s", topo.to_json().c_str());
            return 0;
        }
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            fprintf(stdout,
                "Usage: straylight-fabric [OPTIONS]\n"
                "\n"
                "Unified device topology graph daemon for StrayLight OS.\n"
                "\n"
                "Options:\n"
                "  --dump-json   Scan topology and dump JSON to stdout, then exit\n"
                "  -h, --help    Show this help\n");
            return 0;
        }
    }

    straylight::fabric::FabricDaemon daemon;
    return daemon.run(straylight::Config::make_empty());
}
