// services/replay/replay_daemon.h
// Daemon wrapper for the event flight recorder.
#pragma once

#include <straylight/daemon.h>
#include "recorder.h"
#include "analyzer.h"

namespace straylight {

/// Replay daemon — continuously records system events into a ring buffer.
/// Exposes event query and crash analysis through IPC and D-Bus.
class ReplayDaemon : public DaemonBase {
public:
    Result<void, SLError> init(const Config& cfg) override;
    Result<void, SLError> tick() override;
    void shutdown() override;

private:
    EventRecorder recorder_;
    size_t buffer_size_mb_ = 256;
    int ipc_fd_ = -1;
    std::string socket_path_ = "/run/straylight/replay.sock";

    /// Set up the IPC socket for CLI clients.
    Result<void, SLError> setup_ipc();

    /// Handle one incoming IPC request.
    void handle_ipc_request(int client_fd);

    /// Process a JSON-RPC method call and return the response.
    std::string dispatch_rpc(const std::string& request_json);
};

} // namespace straylight
