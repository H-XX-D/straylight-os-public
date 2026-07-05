// services/remote/tls_server.h
#pragma once

#include <straylight/result.h>
#include <openssl/ssl.h>
#include <openssl/evp.h>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <chrono>

namespace straylight {

/// Represents a single authenticated TLS session.
struct TlsSession {
    SSL* ssl = nullptr;
    int fd = -1;
    std::string client_id;  // Comment from authorized_keys
    std::chrono::steady_clock::time_point last_activity;
    std::string recv_buffer;
    bool authenticated = false;
    int auth_attempts = 0;
};

/// TLS 1.3 server with Ed25519 key-based authentication.
/// Uses epoll for non-blocking I/O. Supports up to max_connections concurrent clients.
/// Reads authorized_keys file for Ed25519 public key verification.
class TlsServer {
public:
    TlsServer();
    ~TlsServer();

    TlsServer(const TlsServer&) = delete;
    TlsServer& operator=(const TlsServer&) = delete;

    /// Initialize the TLS server. Loads certs, keys, authorized_keys, binds and listens.
    Result<void, std::string> init(const std::string& cert_path,
                                    const std::string& key_path,
                                    const std::string& auth_keys_path,
                                    const std::string& bind_addr,
                                    int port,
                                    int max_connections,
                                    int idle_timeout_s);

    /// Set the callback invoked for each JSON-RPC request.
    /// The callback receives the raw JSON string and returns a JSON response string.
    using RequestHandler = std::function<std::string(const std::string&)>;
    void set_request_handler(RequestHandler handler);

    /// Poll for events. timeout_ms is the epoll wait timeout.
    void poll(int timeout_ms);

    /// Gracefully shut down all connections and release resources.
    void stop();

private:
    // OpenSSL context
    SSL_CTX* ssl_ctx_ = nullptr;

    // Listening socket
    int listen_fd_ = -1;

    // Epoll fd (Linux) / kqueue fd (macOS)
    int poll_fd_ = -1;

    // Configuration
    int max_connections_ = 8;
    int idle_timeout_s_ = 600;
    int port_ = 7700;

    // Active sessions keyed by fd
    std::unordered_map<int, std::unique_ptr<TlsSession>> sessions_;
    mutable std::mutex sessions_mutex_;

    // Authorized public keys: base64-encoded key -> comment
    std::vector<std::pair<std::string, std::string>> authorized_keys_;

    // Request handler callback
    RequestHandler request_handler_;

    // Rate limiting for auth attempts: IP -> (count, first_attempt_time)
    struct RateLimit {
        int count = 0;
        std::chrono::steady_clock::time_point window_start;
    };
    std::unordered_map<std::string, RateLimit> auth_rate_limits_;
    static constexpr int kMaxAuthAttemptsPerWindow = 10;
    static constexpr int kRateLimitWindowSeconds = 60;

    // Internal methods
    Result<void, std::string> init_ssl_context(const std::string& cert_path,
                                                const std::string& key_path);
    Result<void, std::string> load_authorized_keys(const std::string& path);
    Result<void, std::string> bind_and_listen(const std::string& addr, int port);
    Result<void, std::string> init_poll();

    void accept_connection();
    void handle_read(int fd);
    void process_message(TlsSession& session, const std::string& message);
    bool authenticate_client(TlsSession& session, const std::string& challenge_response);
    bool is_rate_limited(const std::string& peer_addr);
    void close_session(int fd);
    void reap_idle_sessions();
    std::string get_peer_address(int fd);

    // Frame protocol: 4-byte big-endian length prefix + payload
    static std::string frame_message(const std::string& payload);
    bool try_extract_frame(std::string& buffer, std::string& out_frame);
};

} // namespace straylight
