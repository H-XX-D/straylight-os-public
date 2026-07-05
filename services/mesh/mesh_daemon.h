// services/mesh/mesh_daemon.h
// StrayLight Mesh — Daemon combining GpuPool + MeshMonitor + D-Bus interface.
#pragma once

#include <straylight/common.h>
#include <straylight/daemon.h>
#include "gpu_pool.h"
#include "mesh_monitor.h"

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <sys/epoll.h>

#include <nlohmann/json.hpp>

namespace straylight {

class MeshDaemon : public DaemonBase {
public:
    Result<void, SLError> init(const Config& cfg) override;
    Result<void, SLError> tick() override;
    void shutdown() override;

    // D-Bus method handlers
    std::string dbus_pool_status() const;
    std::string dbus_list_gpus() const;
    std::string dbus_submit(const std::string& command, size_t vram_needed);
    std::string dbus_allocate(size_t bytes, const std::string& policy);
    std::string dbus_free(uint64_t handle, const std::string& host, uint32_t gpu_index);
    std::string dbus_transfer(const std::string& src_host, uint32_t src_gpu,
                               uint64_t src_handle,
                               const std::string& dst_host, uint32_t dst_gpu,
                               uint64_t dst_handle, size_t bytes);

private:
    GpuPool pool_;
    std::unique_ptr<MeshMonitor> monitor_;

    int refresh_interval_s_ = 5;
    int discover_interval_s_ = 60;
    bool fabric_enabled_ = true;
    int fabric_refresh_interval_s_ = 30;
    std::string fabric_socket_path_ = "/run/straylight/fabric.sock";

    std::chrono::steady_clock::time_point last_refresh_;
    std::chrono::steady_clock::time_point last_discover_;
    std::chrono::steady_clock::time_point last_fabric_refresh_;

    mutable std::mutex mutex_;
    mutable std::mutex placement_mutex_;
    nlohmann::json placement_snapshot_;

    // ── IPC socket ───────────────────────────────────────────────────
    std::string socket_path_ = "/run/straylight/mesh.sock";
    int server_fd_  = -1;
    int epoll_fd_   = -1;
    std::vector<int> client_fds_;

    Result<void, SLError> setup_ipc();
    void teardown_ipc();
    void poll_ipc(int timeout_ms);
    void handle_accept();
    void handle_client(int fd);
    void remove_client(int fd);
    std::optional<std::string> recv_frame(int fd);
    bool send_frame(int fd, const std::string& data);
    void send_to_all(const std::string& data);
    nlohmann::json build_pool_payload() const;
    nlohmann::json placement_snapshot() const;
    nlohmann::json placement_summary() const;
    nlohmann::json placement_hint_for_gpu(const RemoteGpu& gpu) const;
    void refresh_fabric_placement(const char* reason);
    void maybe_refresh_fabric_placement();
    nlohmann::json dispatch(const nlohmann::json& msg);
};

} // namespace straylight
