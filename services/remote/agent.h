// services/remote/agent.h
#pragma once

#include <straylight/daemon.h>
#include <straylight/config.h>
#include <straylight/result.h>
#include <straylight/error.h>
#include <memory>
#include <string>

namespace straylight {

class TlsServer;
class CommandHandler;

/// Remote management agent daemon.
/// Listens for TLS-encrypted JSON-RPC 2.0 connections on port 7700 (configurable).
/// Provides command execution, file transfer, system info, service management,
/// log streaming, process management, package management, and Alice integration.
class RemoteAgent : public DaemonBase {
public:
    RemoteAgent();
    ~RemoteAgent() override;

    Result<void, SLError> init(const Config& cfg) override;
    Result<void, SLError> tick() override;
    void shutdown() override;

private:
    std::unique_ptr<TlsServer> tls_server_;
    std::unique_ptr<CommandHandler> command_handler_;
    int idle_timeout_s_ = 600;
    int max_exec_timeout_s_ = 3600;
};

} // namespace straylight
