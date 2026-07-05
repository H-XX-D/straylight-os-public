// bin/bus/bus_daemon.h
#pragma once

#include <straylight/common.h>
#include <straylight/daemon.h>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <sys/epoll.h>

namespace straylight {

using SignalHandler = std::function<void(const std::string&)>;

class BusDaemon : public DaemonBase {
public:
    Result<void, SLError> init(const Config& cfg) override;
    Result<void, SLError> tick() override;
    void shutdown() override;

    // Service registry (name -> owner pid)
    Result<void, SLError> register_service(const std::string& name, pid_t owner);
    void unregister_service(const std::string& name);
    std::optional<pid_t> lookup_owner(const std::string& name) const;

    // Signal forwarding
    void subscribe(const std::string& service, const std::string& signal,
                   SignalHandler handler);
    void emit(const std::string& service, const std::string& signal,
              const std::string& payload);

private:
    std::unordered_map<std::string, pid_t> service_registry_;
    std::unordered_map<std::string, std::vector<SignalHandler>> subscriptions_;
    mutable std::mutex mutex_;

    // IPC socket server
    std::string socket_path_ = "/run/straylight/bus.sock";
    int server_fd_ = -1;
    int epoll_fd_ = -1;
    std::vector<int> client_fds_;
    uint64_t messages_routed_ = 0;

    Result<void, SLError> setup_ipc();
    void teardown_ipc();
    void poll_ipc(int timeout_ms);
    void handle_accept();
    void handle_client(int fd, uint32_t events);
    void remove_client(int fd);

    // Length-prefixed framing (4-byte LE length + UTF-8 JSON)
    Result<std::string, std::string> recv_frame(int fd);
    void send_frame(int fd, const std::string& msg);

    // JSON-RPC dispatch
    std::string dispatch(const std::string& json_request);
};

} // namespace straylight
