// bin/core/core_daemon.h
#pragma once

#include <straylight/common.h>
#include <straylight/daemon.h>
#include "pipeline.h"
#include "doctor.h"
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>
#include <sys/epoll.h>

namespace straylight {

class CoreDaemon : public DaemonBase {
public:
    Result<void, SLError> init(const Config& cfg) override;
    Result<void, SLError> tick() override;
    void shutdown() override;

    void register_subsystem(const std::string& name, SubsystemPriority prio);
    void on_health_update(const std::string& name, HealthStatus status);
    bool is_ready() const;

private:
    Pipeline pipeline_;
    Doctor doctor_;
    int poll_interval_s_ = 10;
    int restart_max_ = 5;
    std::unordered_map<std::string, int> restart_counts_;
    bool ready_ = false;
    mutable std::mutex mutex_;  // Guards pipeline_, doctor_, restart_counts_, ready_

    void check_readiness();

    // IPC socket server (exposes /run/straylight/core.sock)
    std::string socket_path_ = "/run/straylight/core.sock";
    int server_fd_ = -1;
    int epoll_fd_ = -1;
    std::vector<int> client_fds_;

    Result<void, SLError> setup_ipc();
    void teardown_ipc();
    void poll_ipc(int timeout_ms);
    void handle_accept();
    void handle_client(int fd);
    void remove_client(int fd);
    std::optional<std::string> recv_frame(int fd);
    bool send_frame(int fd, const std::string& msg);
    void send_to_all(const std::string& data);
    nlohmann::json build_state_payload();
    nlohmann::json dispatch(const nlohmann::json& msg);
};

} // namespace straylight
