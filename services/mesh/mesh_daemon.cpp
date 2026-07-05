// services/mesh/mesh_daemon.cpp
// StrayLight Mesh — Daemon implementation.
#include "mesh_daemon.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <sstream>

#include <fcntl.h>
#include <grp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace straylight {

static int64_t mesh_now_epoch_ms() {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

static bool mesh_write_all(int fd, const void* data, size_t len) {
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

static bool mesh_read_exact(int fd, void* data, size_t len) {
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

static Result<nlohmann::json, std::string> mesh_recv_json_frame(int fd) {
    uint32_t len = 0;
    if (!mesh_read_exact(fd, &len, sizeof(len))) {
        return Result<nlohmann::json, std::string>::error("fabric frame header read failed");
    }
    if (len == 0 || len > 4 * 1024 * 1024) {
        return Result<nlohmann::json, std::string>::error("fabric frame length invalid");
    }

    std::string body(len, '\0');
    if (!mesh_read_exact(fd, body.data(), body.size())) {
        return Result<nlohmann::json, std::string>::error("fabric frame body read failed");
    }

    try {
        return Result<nlohmann::json, std::string>::ok(nlohmann::json::parse(body));
    } catch (const std::exception& e) {
        return Result<nlohmann::json, std::string>::error(
            std::string("fabric JSON parse failed: ") + e.what());
    }
}

static Result<nlohmann::json, std::string> mesh_fabric_request(
    const std::string& socket_path,
    const std::string& method) {
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
    ::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        int e = errno;
        ::close(fd);
        return Result<nlohmann::json, std::string>::error(
            std::string("connect() to fabric failed: ") + ::strerror(e));
    }

    nlohmann::json req;
    req["id"] = "mesh-placement";
    req["method"] = method;
    std::string payload = req.dump();
    uint32_t len = static_cast<uint32_t>(payload.size());

    if (!mesh_write_all(fd, &len, sizeof(len)) ||
        !mesh_write_all(fd, payload.data(), payload.size())) {
        ::close(fd);
        return Result<nlohmann::json, std::string>::error("fabric request write failed");
    }

    for (int i = 0; i < 8; ++i) {
        auto frame = mesh_recv_json_frame(fd);
        if (!frame.has_value()) {
            ::close(fd);
            return frame;
        }

        auto msg = frame.value();
        if (msg.value("type", "") == "res" &&
            msg.value("id", "") == "mesh-placement") {
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

static std::string mesh_normalize_bdf(std::string bdf) {
    if (bdf.rfind("00000000:", 0) == 0) {
        bdf = "0000:" + bdf.substr(9);
    }
    return bdf;
}

static std::string mesh_local_gpu_bdf(const RemoteGpu& gpu) {
    if (!gpu.is_local || gpu.name.rfind("card", 0) != 0) return {};
    std::filesystem::path device_link =
        std::filesystem::path("/sys/class/drm") / gpu.name / "device";
    std::error_code ec;
    auto resolved = std::filesystem::canonical(device_link, ec);
    if (ec) return {};
    return resolved.filename().string();
}

static std::string mesh_node_placement_plane(const nlohmann::json& node) {
    if (node.contains("properties") && node["properties"].is_object()) {
        return node["properties"].value("placement_plane", "");
    }
    return {};
}

static nlohmann::json mesh_accelerator_hint_from_node(
    const nlohmann::json& node,
    const std::string& source) {
    nlohmann::json hint;
    hint["source"] = source;
    hint["policy"] = "observe_only";
    hint["fabric_node_id"] = node.value("id", "");
    hint["fabric_node_name"] = node.value("name", "");
    hint["fabric_node_type"] = node.value("type", "");
    hint["numa_node"] = node.value("numa_node", -1);

    std::string plane = mesh_node_placement_plane(node);
    if (!plane.empty()) hint["placement_plane"] = plane;

    if (node.contains("properties") && node["properties"].is_object()) {
        const auto& props = node["properties"];
        if (props.contains("bdf")) {
            hint["bdf"] = props.value("bdf", "");
        } else if (props.contains("pci_bus")) {
            hint["bdf"] = mesh_normalize_bdf(props.value("pci_bus", ""));
        }
        if (props.contains("link_speed")) hint["link_speed"] = props.value("link_speed", "");
        if (props.contains("link_width")) hint["link_width"] = props.value("link_width", "");
    }

    return hint;
}

// ---------------------------------------------------------------------------
// DaemonBase overrides
// ---------------------------------------------------------------------------

Result<void, SLError> MeshDaemon::init(const Config& cfg) {
    SL_INFO("mesh: initializing daemon");

    refresh_interval_s_  = cfg.get<int>("refresh_interval_seconds", 5);
    discover_interval_s_ = cfg.get<int>("discover_interval_seconds", 60);
    fabric_enabled_ = cfg.get<bool>("fabric.enabled", true);
    fabric_socket_path_ = cfg.get<std::string>(
        "fabric.socket_path", "/run/straylight/fabric.sock");
    fabric_refresh_interval_s_ = cfg.get<int>("fabric.refresh_interval_s", 30);

    {
        std::lock_guard<std::mutex> lock(placement_mutex_);
        placement_snapshot_ = {
            {"schema_version", "straylight.mesh.placement.v1"},
            {"fabric_schema_version", "straylight.fabric.placement.v1"},
            {"owner", "mesh"},
            {"source", "fabric"},
            {"policy", "observe_only"},
            {"connected", false},
            {"locality_islands", nlohmann::json::array()},
            {"accelerators", nlohmann::json::array()}
        };
    }

    float temp_threshold = cfg.get<float>("temperature_threshold_celsius", 90.0f);
    float util_threshold = cfg.get<float>("utilization_threshold_percent", 95.0f) / 100.0f;
    int stale_timeout    = cfg.get<int>("stale_timeout_seconds", 30);

    // Initial discovery
    auto disc_result = pool_.discover();
    if (!disc_result.has_value()) {
        SL_WARN("mesh: initial discovery partially failed: {}", disc_result.error());
    }

    // Start monitor
    monitor_ = std::make_unique<MeshMonitor>(pool_);
    monitor_->set_temperature_threshold(temp_threshold);
    monitor_->set_utilization_threshold(util_threshold);
    monitor_->set_stale_timeout(std::chrono::seconds(stale_timeout));

    // Set alert callback to emit D-Bus signals
    monitor_->set_alert_callback([](const MeshAlert& alert) {
        // In production, this would emit a D-Bus signal on org.straylight.Mesh1
        // For now, just log it
        SL_INFO("mesh-alert: {}", alert.message);
    });

    auto mon_result = monitor_->start(std::chrono::seconds(refresh_interval_s_));
    if (!mon_result.has_value()) {
        SL_ERROR("mesh: failed to start monitor: {}", mon_result.error());
        return Result<void, SLError>::error(
            SLError{SLErrorCode::Internal, mon_result.error()});
    }

    auto now = std::chrono::steady_clock::now();
    last_refresh_  = now;
    last_discover_ = now;
    last_fabric_refresh_ = now - std::chrono::seconds(3600);

    refresh_fabric_placement("initial");

    SL_INFO("mesh: daemon initialized with {} GPU(s)", pool_.gpu_count());

    auto ipc_res = setup_ipc();
    if (!ipc_res.has_value())
        SL_WARN("mesh: IPC socket not available: {}", ipc_res.error().message());

    return Result<void, SLError>::ok();
}

Result<void, SLError> MeshDaemon::tick() {
    auto now = std::chrono::steady_clock::now();

    // Periodic re-discovery of new nodes
    auto since_discover = std::chrono::duration_cast<std::chrono::seconds>(
        now - last_discover_);
    if (since_discover.count() >= discover_interval_s_) {
        last_discover_ = now;
        SL_DEBUG("mesh: running periodic discovery");
        pool_.discover();
    }

    maybe_refresh_fabric_placement();

    // Sleep briefly to avoid busy-spinning
    poll_ipc(80);
    usleep(20000); // 20ms remainder

    return Result<void, SLError>::ok();
}

void MeshDaemon::shutdown() {
    SL_INFO("mesh: shutting down");
    teardown_ipc();

    if (monitor_) {
        monitor_->stop();
        monitor_.reset();
    }

    SL_INFO("mesh: shutdown complete");
}

// ---------------------------------------------------------------------------
// D-Bus method handlers
// ---------------------------------------------------------------------------

std::string MeshDaemon::dbus_pool_status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (monitor_) {
        return monitor_->status_summary();
    }
    return "Monitor not running";
}

std::string MeshDaemon::dbus_list_gpus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto gpus = pool_.all_gpus();

    std::ostringstream out;
    out << "HOST             GPU  NAME                 VENDOR     VRAM(GiB)  FREE(GiB)  TEMP  UTIL  LATENCY\n";
    out << "---------------  ---  -------------------  ---------  ---------  ---------  ----  ----  -------\n";

    for (const auto& gpu : gpus) {
        char line[256];
        std::snprintf(line, sizeof(line),
            "%-15s  %3u  %-19s  %-9s  %9.1f  %9.1f  %3.0fC  %3.0f%%  %5.1fms%s\n",
            gpu.host.c_str(),
            gpu.gpu_index,
            gpu.name.c_str(),
            gpu.vendor.c_str(),
            static_cast<double>(gpu.vram_total) / (1024.0 * 1024.0 * 1024.0),
            static_cast<double>(gpu.vram_available) / (1024.0 * 1024.0 * 1024.0),
            static_cast<double>(gpu.temperature),
            static_cast<double>(gpu.utilization * 100.0f),
            static_cast<double>(gpu.latency_ms),
            gpu.is_available ? "" : " [UNAVAIL]");
        out << line;
    }

    return out.str();
}

std::string MeshDaemon::dbus_submit(const std::string& command, size_t vram_needed) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = pool_.submit(command, vram_needed);
    if (result.has_value()) {
        return result.value();
    }
    return "ERROR: " + result.error();
}

std::string MeshDaemon::dbus_allocate(size_t bytes, const std::string& policy_str) {
    std::lock_guard<std::mutex> lock(mutex_);

    PlacementPolicy policy = PlacementPolicy::LeastLoaded;
    if (policy_str == "best_fit")      policy = PlacementPolicy::BestFit;
    else if (policy_str == "local")    policy = PlacementPolicy::LocalFirst;
    else if (policy_str == "round")    policy = PlacementPolicy::RoundRobin;
    else if (policy_str == "pinned")   policy = PlacementPolicy::Pinned;

    auto result = pool_.allocate(bytes, policy);
    if (result.has_value()) {
        const auto& alloc = result.value();
        return std::to_string(alloc.handle) + " " + alloc.host + " " +
               std::to_string(alloc.gpu_index);
    }
    return "ERROR: " + result.error();
}

std::string MeshDaemon::dbus_free(uint64_t handle, const std::string& host,
                                    uint32_t gpu_index) {
    std::lock_guard<std::mutex> lock(mutex_);

    MeshAllocation alloc;
    alloc.handle    = handle;
    alloc.host      = host;
    alloc.gpu_index = gpu_index;
    alloc.is_local  = (host == "localhost" || host == "127.0.0.1");

    auto result = pool_.free(alloc);
    if (result.has_value()) {
        return "OK";
    }
    return "ERROR: " + result.error();
}

std::string MeshDaemon::dbus_transfer(const std::string& src_host, uint32_t src_gpu,
                                        uint64_t src_handle,
                                        const std::string& dst_host, uint32_t dst_gpu,
                                        uint64_t dst_handle, size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);

    MeshAllocation src;
    src.handle    = src_handle;
    src.host      = src_host;
    src.gpu_index = src_gpu;
    src.size_bytes = bytes;
    src.is_local  = (src_host == "localhost" || src_host == "127.0.0.1");

    MeshAllocation dst;
    dst.handle    = dst_handle;
    dst.host      = dst_host;
    dst.gpu_index = dst_gpu;
    dst.size_bytes = bytes;
    dst.is_local  = (dst_host == "localhost" || dst_host == "127.0.0.1");

    auto result = pool_.transfer(src, dst);
    if (result.has_value()) {
        return "OK";
    }
    return "ERROR: " + result.error();
}

} // namespace straylight

// ─── IPC socket (after closing brace to avoid redefining namespace too early)
// Reopened below:
namespace straylight {

static bool mesh_set_nonblocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

Result<void, SLError> MeshDaemon::setup_ipc() {
    std::filesystem::create_directories("/run/straylight");

    server_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd_ < 0)
        return Result<void, SLError>::error(SLError{SLErrorCode::IOError, std::strerror(errno)});

    mesh_set_nonblocking(server_fd_);

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    ::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);
    ::unlink(socket_path_.c_str());

    if (::bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(server_fd_); server_fd_ = -1;
        return Result<void, SLError>::error(SLError{SLErrorCode::IOError, std::strerror(errno)});
    }
    ::listen(server_fd_, 8);

    if (auto* group = ::getgrnam("straylight")) {
        ::chown(socket_path_.c_str(), 0, group->gr_gid);
    }
    ::chmod(socket_path_.c_str(), 0660);

    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        ::close(server_fd_); server_fd_ = -1;
        return Result<void, SLError>::error(SLError{SLErrorCode::IOError, std::strerror(errno)});
    }

    epoll_event ev{};
    ev.events  = EPOLLIN;
    ev.data.fd = server_fd_;
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, server_fd_, &ev);

    SL_INFO("mesh: IPC listening on {}", socket_path_);
    return Result<void, SLError>::ok();
}

void MeshDaemon::teardown_ipc() {
    for (int fd : client_fds_) ::close(fd);
    client_fds_.clear();
    if (epoll_fd_ >= 0) { ::close(epoll_fd_); epoll_fd_ = -1; }
    if (server_fd_ >= 0) { ::close(server_fd_); server_fd_ = -1; }
    ::unlink(socket_path_.c_str());
}

void MeshDaemon::poll_ipc(int timeout_ms) {
    if (epoll_fd_ < 0) return;

    epoll_event events[16];
    int n = ::epoll_wait(epoll_fd_, events, 16, timeout_ms);
    for (int i = 0; i < n; ++i) {
        if (events[i].data.fd == server_fd_)
            handle_accept();
        else
            handle_client(events[i].data.fd);
    }

    if (!client_fds_.empty()) {
        nlohmann::json ev;
        ev["service"] = "mesh";
        ev["type"]    = "event";
        ev["payload"] = build_pool_payload();
        send_to_all(ev.dump());
    }
}

void MeshDaemon::handle_accept() {
    sockaddr_un addr{};
    socklen_t len = sizeof(addr);
    int fd = ::accept4(server_fd_, reinterpret_cast<sockaddr*>(&addr), &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) return;

    epoll_event ev{};
    ev.events  = EPOLLIN | EPOLLHUP | EPOLLERR;
    ev.data.fd = fd;
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev);
    client_fds_.push_back(fd);
}

void MeshDaemon::handle_client(int fd) {
    auto frame = recv_frame(fd);
    if (!frame) { remove_client(fd); return; }

    try {
        auto msg   = nlohmann::json::parse(*frame);
        auto reply = dispatch(msg);
        send_frame(fd, reply.dump());
    } catch (...) {
        nlohmann::json err;
        err["service"] = "mesh";
        err["type"]    = "error";
        err["payload"]["message"] = "parse error";
        send_frame(fd, err.dump());
    }
}

void MeshDaemon::remove_client(int fd) {
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    ::close(fd);
    client_fds_.erase(std::remove(client_fds_.begin(), client_fds_.end(), fd), client_fds_.end());
}

std::optional<std::string> MeshDaemon::recv_frame(int fd) {
    uint32_t len_le = 0;
    if (::recv(fd, &len_le, 4, MSG_WAITALL) != 4) return std::nullopt;
    uint32_t len = len_le; // LE on LE host
    if (len == 0 || len > 4 * 1024 * 1024) return std::nullopt;
    std::string buf(len, '\0');
    if (::recv(fd, buf.data(), len, MSG_WAITALL) != static_cast<ssize_t>(len)) return std::nullopt;
    return buf;
}

bool MeshDaemon::send_frame(int fd, const std::string& data) {
    uint32_t len = static_cast<uint32_t>(data.size());
    if (::send(fd, &len, 4, MSG_NOSIGNAL) != 4) return false;
    return ::send(fd, data.data(), len, MSG_NOSIGNAL) == static_cast<ssize_t>(len);
}

void MeshDaemon::send_to_all(const std::string& data) {
    std::vector<int> dead;
    for (int fd : client_fds_)
        if (!send_frame(fd, data)) dead.push_back(fd);
    for (int fd : dead) remove_client(fd);
}

nlohmann::json MeshDaemon::placement_snapshot() const {
    std::lock_guard<std::mutex> lock(placement_mutex_);
    return placement_snapshot_;
}

nlohmann::json MeshDaemon::placement_summary() const {
    auto snapshot = placement_snapshot();
    nlohmann::json p;
    p["schema_version"] = snapshot.value("schema_version", "straylight.mesh.placement.v1");
    p["fabric_schema_version"] =
        snapshot.value("fabric_schema_version", "straylight.fabric.placement.v1");
    p["connected"] = snapshot.value("connected", false);
    p["source"] = snapshot.value("source", "fabric");
    p["policy"] = snapshot.value("policy", "observe_only");
    p["locality_island_count"] =
        snapshot.contains("locality_islands") && snapshot["locality_islands"].is_array()
            ? snapshot["locality_islands"].size()
            : 0;
    p["accelerator_count"] =
        snapshot.contains("accelerators") && snapshot["accelerators"].is_array()
            ? snapshot["accelerators"].size()
            : 0;
    if (snapshot.contains("last_error")) p["last_error"] = snapshot["last_error"];
    if (snapshot.contains("last_refresh_epoch_ms")) {
        p["last_refresh_epoch_ms"] = snapshot["last_refresh_epoch_ms"];
    }
    return p;
}

nlohmann::json MeshDaemon::placement_hint_for_gpu(const RemoteGpu& gpu) const {
    auto snapshot = placement_snapshot();
    if (!snapshot.value("connected", false) ||
        !snapshot.contains("accelerators") ||
        !snapshot["accelerators"].is_array()) {
        return nlohmann::json::object();
    }

    std::string local_bdf = mesh_local_gpu_bdf(gpu);
    const auto& accelerators = snapshot["accelerators"];

    auto find_by_bdf = [&](const std::string& bdf) -> nlohmann::json {
        if (bdf.empty()) return nlohmann::json::object();
        for (const auto& node : accelerators) {
            if (!node.contains("properties") || !node["properties"].is_object()) continue;
            const auto& props = node["properties"];
            if (props.contains("bdf") && props.value("bdf", "") == bdf) {
                return mesh_accelerator_hint_from_node(node, "fabric");
            }
        }
        for (const auto& node : accelerators) {
            if (!node.contains("properties") || !node["properties"].is_object()) continue;
            const auto& props = node["properties"];
            if (props.contains("pci_bus") &&
                mesh_normalize_bdf(props.value("pci_bus", "")) == bdf) {
                return mesh_accelerator_hint_from_node(node, "fabric");
            }
        }
        return nlohmann::json::object();
    };

    auto direct = find_by_bdf(local_bdf);
    if (!direct.empty()) return direct;

    const std::string telemetry_id = "gpu:" + std::to_string(gpu.gpu_index);
    nlohmann::json telemetry_node;
    for (const auto& node : accelerators) {
        if (node.value("id", "") == telemetry_id) {
            telemetry_node = node;
            break;
        }
    }
    if (!telemetry_node.empty()) {
        if (telemetry_node.contains("properties") &&
            telemetry_node["properties"].is_object() &&
            telemetry_node["properties"].contains("pci_bus")) {
            auto pci_bdf = mesh_normalize_bdf(
                telemetry_node["properties"].value("pci_bus", ""));
            auto pci_hint = find_by_bdf(pci_bdf);
            if (!pci_hint.empty()) {
                pci_hint["telemetry_node_id"] = telemetry_node.value("id", "");
                pci_hint["telemetry_placement_plane"] =
                    mesh_node_placement_plane(telemetry_node);
                return pci_hint;
            }
        }
        return mesh_accelerator_hint_from_node(telemetry_node, "fabric");
    }

    nlohmann::json only_pci_gpu;
    size_t pci_gpu_count = 0;
    for (const auto& node : accelerators) {
        if (node.value("type", "") != "gpu") continue;
        if (node.value("id", "").rfind("pci:", 0) == 0) {
            only_pci_gpu = node;
            ++pci_gpu_count;
        }
    }
    if (gpu.is_local && pci_gpu_count == 1) {
        return mesh_accelerator_hint_from_node(only_pci_gpu, "fabric");
    }

    return nlohmann::json::object();
}

void MeshDaemon::refresh_fabric_placement(const char* reason) {
    if (!fabric_enabled_) return;

    auto status = mesh_fabric_request(fabric_socket_path_, "status");
    auto refreshed_at = mesh_now_epoch_ms();

    if (!status.has_value()) {
        std::lock_guard<std::mutex> lock(placement_mutex_);
        placement_snapshot_["connected"] = false;
        placement_snapshot_["last_error"] = status.error();
        placement_snapshot_["last_refresh_epoch_ms"] = refreshed_at;
        last_fabric_refresh_ = std::chrono::steady_clock::now();
        SL_WARN("mesh: Fabric placement refresh ({}) failed: {}", reason, status.error());
        return;
    }

    nlohmann::json topology;
    try {
        const auto& payload = status.value();
        if (payload.contains("topology") && payload["topology"].is_string()) {
            topology = nlohmann::json::parse(payload["topology"].get<std::string>());
        }
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(placement_mutex_);
        placement_snapshot_["connected"] = false;
        placement_snapshot_["last_error"] =
            std::string("fabric topology parse failed: ") + e.what();
        placement_snapshot_["last_refresh_epoch_ms"] = refreshed_at;
        last_fabric_refresh_ = std::chrono::steady_clock::now();
        SL_WARN("mesh: Fabric placement refresh ({}) parse failed: {}", reason, e.what());
        return;
    }

    nlohmann::json accelerators = nlohmann::json::array();
    nlohmann::json local_storage = nlohmann::json::array();
    if (topology.contains("nodes") && topology["nodes"].is_array()) {
        for (const auto& node : topology["nodes"]) {
            std::string type = node.value("type", "");
            if (type == "gpu" || type == "vpu" || type == "fpga") {
                accelerators.push_back(node);
            } else if (type == "nvme") {
                local_storage.push_back(node);
            }
        }
    }

    nlohmann::json snapshot;
    snapshot["schema_version"] = "straylight.mesh.placement.v1";
    snapshot["fabric_schema_version"] = "straylight.fabric.placement.v1";
    snapshot["owner"] = "mesh";
    snapshot["source"] = "fabric";
    snapshot["policy"] = "observe_only";
    snapshot["connected"] = true;
    snapshot["socket_path"] = fabric_socket_path_;
    snapshot["last_refresh_epoch_ms"] = refreshed_at;
    snapshot["device_count"] = status.value().value("device_count", 0);
    snapshot["link_count"] = status.value().value("link_count", 0);
    snapshot["locality_islands"] =
        topology.value("locality_islands", nlohmann::json::array());
    snapshot["accelerators"] = std::move(accelerators);
    snapshot["local_storage"] = std::move(local_storage);

    {
        std::lock_guard<std::mutex> lock(placement_mutex_);
        placement_snapshot_ = std::move(snapshot);
    }

    last_fabric_refresh_ = std::chrono::steady_clock::now();
    SL_INFO("mesh: Fabric placement refresh ({}) saw {} island(s), {} accelerator node(s)",
            reason,
            placement_summary().value("locality_island_count", size_t(0)),
            placement_summary().value("accelerator_count", size_t(0)));
}

void MeshDaemon::maybe_refresh_fabric_placement() {
    if (!fabric_enabled_) return;

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - last_fabric_refresh_);
    if (elapsed.count() >= fabric_refresh_interval_s_) {
        refresh_fabric_placement("periodic");
    }
}

nlohmann::json MeshDaemon::build_pool_payload() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto gpus = pool_.all_gpus();
    nlohmann::json p;
    p["gpu_count"]   = gpus.size();
    p["gpus"]        = nlohmann::json::array();
    p["placement"]   = placement_summary();
    size_t total_vram = 0, free_vram = 0;
    for (const auto& g : gpus) {
        nlohmann::json gj;
        gj["host"]         = g.host;
        gj["gpu_index"]    = g.gpu_index;
        gj["name"]         = g.name;
        gj["vendor"]       = g.vendor;
        gj["vram_total"]   = g.vram_total;
        gj["vram_free"]    = g.vram_available;
        gj["temp_c"]       = g.temperature;
        gj["util_pct"]     = static_cast<int>(g.utilization * 100.0f);
        gj["available"]    = g.is_available;
        auto hint = placement_hint_for_gpu(g);
        if (!hint.empty()) {
            gj["placement"] = hint;
            if (hint.contains("placement_plane")) {
                gj["placement_plane"] = hint["placement_plane"];
            }
            if (hint.contains("numa_node")) gj["numa_node"] = hint["numa_node"];
            if (hint.contains("bdf")) gj["bdf"] = hint["bdf"];
        }
        total_vram += g.vram_total;
        free_vram  += g.vram_available;
        p["gpus"].push_back(gj);
    }
    p["total_vram_bytes"] = total_vram;
    p["free_vram_bytes"]  = free_vram;
    return p;
}

nlohmann::json MeshDaemon::dispatch(const nlohmann::json& msg) {
    nlohmann::json r;
    r["service"] = "mesh";
    r["type"]    = "res";
    if (msg.contains("id")) r["id"] = msg["id"];

    std::string method = msg.value("method", "");

    if (method == "status" || method == "node_list") {
        r["payload"] = build_pool_payload();
    } else if (method == "list_gpus") {
        r["payload"]["gpus"] = build_pool_payload()["gpus"];
    } else if (method == "placement_report") {
        r["payload"] = placement_snapshot();
    } else if (method == "allocate") {
        size_t bytes = msg.value("bytes", size_t(0));
        std::string pol_str = msg.value("policy", "least_loaded");
        PlacementPolicy pol = PlacementPolicy::LeastLoaded;
        if (pol_str == "best_fit")   pol = PlacementPolicy::BestFit;
        else if (pol_str == "local") pol = PlacementPolicy::LocalFirst;
        else if (pol_str == "round") pol = PlacementPolicy::RoundRobin;
        std::lock_guard<std::mutex> lock(mutex_);
        auto res = pool_.allocate(bytes, pol);
        if (res.has_value()) {
            r["payload"]["handle"]    = res.value().handle;
            r["payload"]["host"]      = res.value().host;
            r["payload"]["gpu_index"] = res.value().gpu_index;
        } else {
            r["type"] = "error";
            r["payload"]["message"] = res.error();
        }
    } else if (method == "free") {
        MeshAllocation alloc;
        alloc.handle = msg.value("handle", uint64_t(0));
        alloc.host = msg.value("host", "localhost");
        alloc.gpu_index = msg.value("gpu_index", uint32_t(0));
        alloc.size_bytes = msg.value("bytes", size_t(0));
        alloc.is_local = (alloc.host == "localhost" || alloc.host == "127.0.0.1");

        std::lock_guard<std::mutex> lock(mutex_);
        auto res = pool_.free(alloc);
        if (res.has_value()) {
            r["payload"]["status"] = "ok";
        } else {
            r["type"] = "error";
            r["payload"]["message"] = res.error();
        }
    } else if (method == "transfer") {
        size_t bytes = msg.value("bytes", size_t(0));

        MeshAllocation src;
        src.handle = msg.value("src_handle", uint64_t(0));
        src.host = msg.value("src_host", "localhost");
        src.gpu_index = msg.value("src_gpu", uint32_t(0));
        src.size_bytes = bytes;
        src.is_local = (src.host == "localhost" || src.host == "127.0.0.1");

        MeshAllocation dst;
        dst.handle = msg.value("dst_handle", uint64_t(0));
        dst.host = msg.value("dst_host", "localhost");
        dst.gpu_index = msg.value("dst_gpu", uint32_t(0));
        dst.size_bytes = bytes;
        dst.is_local = (dst.host == "localhost" || dst.host == "127.0.0.1");

        std::lock_guard<std::mutex> lock(mutex_);
        auto res = pool_.transfer(src, dst);
        if (res.has_value()) {
            r["payload"]["status"] = "ok";
            r["payload"]["bytes"] = bytes;
        } else {
            r["type"] = "error";
            r["payload"]["message"] = res.error();
        }
    } else {
        r["type"] = "error";
        r["payload"]["message"] = "unknown method: " + method;
    }

    return r;
}

} // namespace straylight
