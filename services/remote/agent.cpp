// services/remote/agent.cpp
#include "agent.h"
#include "tls_server.h"
#include "command_handler.h"
#include <straylight/log.h>
#include <thread>
#include <chrono>

namespace straylight {

RemoteAgent::RemoteAgent() = default;
RemoteAgent::~RemoteAgent() = default;

Result<void, SLError> RemoteAgent::init(const Config& cfg) {
    // Read configuration
    auto listen_addr = cfg.get<std::string>("listen.address", "0.0.0.0");
    auto listen_port = cfg.get<int>("listen.port", 7700);
    auto cert_path = cfg.get<std::string>("tls.cert", "/etc/straylight/remote/server.crt");
    auto key_path = cfg.get<std::string>("tls.key", "/etc/straylight/remote/server.key");
    auto auth_keys_path = cfg.get<std::string>("tls.authorized_keys",
        "/etc/straylight/remote/authorized_keys");
    auto max_connections = cfg.get<int>("limits.max_connections", 8);
    idle_timeout_s_ = cfg.get<int>("limits.idle_timeout_seconds", 600);
    max_exec_timeout_s_ = cfg.get<int>("limits.max_exec_timeout_seconds", 3600);
    auto max_upload_mb = cfg.get<int>("limits.max_upload_size_mb", 1024);

    // Read feature flags
    bool feat_exec = cfg.get<bool>("features.exec", true);
    bool feat_file_transfer = cfg.get<bool>("features.file_transfer", true);
    bool feat_service_mgmt = cfg.get<bool>("features.service_management", true);
    bool feat_package_mgmt = cfg.get<bool>("features.package_management", false);
    bool feat_alice = cfg.get<bool>("features.alice_integration", true);

    // Initialize command handler with feature flags and limits
    command_handler_ = std::make_unique<CommandHandler>();
    command_handler_->set_feature_exec(feat_exec);
    command_handler_->set_feature_file_transfer(feat_file_transfer);
    command_handler_->set_feature_service_management(feat_service_mgmt);
    command_handler_->set_feature_package_management(feat_package_mgmt);
    command_handler_->set_feature_alice(feat_alice);
    command_handler_->set_max_upload_bytes(static_cast<size_t>(max_upload_mb) * 1024 * 1024);
    command_handler_->set_max_exec_timeout(max_exec_timeout_s_);

    // Initialize TLS server
    tls_server_ = std::make_unique<TlsServer>();
    auto tls_result = tls_server_->init(cert_path, key_path, auth_keys_path,
                                         listen_addr, listen_port,
                                         max_connections, idle_timeout_s_);
    if (!tls_result.has_value()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::Internal, tls_result.error()});
    }

    // Set the request handler callback
    tls_server_->set_request_handler(
        [this](const std::string& request) -> std::string {
            return command_handler_->handle(request);
        });

    SL_INFO("remote-agent: initialized on {}:{} (max_conn={}, idle_timeout={}s)",
            listen_addr, listen_port, max_connections, idle_timeout_s_);
    SL_INFO("remote-agent: features: exec={} file_transfer={} services={} packages={} alice={}",
            feat_exec, feat_file_transfer, feat_service_mgmt, feat_package_mgmt, feat_alice);

    return Result<void, SLError>::ok();
}

Result<void, SLError> RemoteAgent::tick() {
    // Process pending connections and handle I/O events.
    // The TLS server uses epoll internally; poll() checks for new connections
    // and processes data on existing ones, with a short timeout to stay responsive.
    tls_server_->poll(100);

    return Result<void, SLError>::ok();
}

void RemoteAgent::shutdown() {
    SL_INFO("remote-agent: shutting down");
    if (tls_server_) {
        tls_server_->stop();
    }
    SL_INFO("remote-agent: shutdown complete");
}

} // namespace straylight
