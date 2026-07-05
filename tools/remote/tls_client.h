// tools/remote/tls_client.h
#pragma once

#include <straylight/result.h>
#include <openssl/ssl.h>
#include <openssl/evp.h>
#include <string>
#include <vector>
#include <chrono>
#include <functional>

namespace straylight {

/// TLS 1.3 client for connecting to straylight-remote-agent.
/// Handles Ed25519 key loading, authentication, reconnection with backoff,
/// and length-prefixed JSON-RPC framing.
class TlsClient {
public:
    TlsClient();
    ~TlsClient();

    TlsClient(const TlsClient&) = delete;
    TlsClient& operator=(const TlsClient&) = delete;

    /// Connect to a remote agent.
    Result<void, std::string> connect(const std::string& host, int port);

    /// Authenticate with the server using an Ed25519 private key.
    Result<void, std::string> authenticate(const std::string& key_path);

    /// Send a JSON-RPC request and receive the response.
    Result<std::string, std::string> request(const std::string& method,
                                               const std::string& params_json,
                                               int timeout_ms = 30000);

    /// Send a raw framed message.
    Result<void, std::string> send(const std::string& message);

    /// Receive a single framed message.
    Result<std::string, std::string> receive(int timeout_ms = 30000);

    /// Check if connected.
    bool is_connected() const { return connected_; }

    /// Close the connection.
    void disconnect();

    /// Generate an Ed25519 keypair and write to files.
    static Result<void, std::string> generate_keypair(const std::string& private_key_path,
                                                        const std::string& public_key_path);

    /// Read a public key file and return the base64-encoded key.
    static Result<std::string, std::string> read_public_key(const std::string& path);

private:
    SSL_CTX* ssl_ctx_ = nullptr;
    SSL* ssl_ = nullptr;
    int fd_ = -1;
    bool connected_ = false;
    int request_id_ = 1;

    // Connection parameters for reconnection
    std::string host_;
    int port_ = 7700;
    std::string key_path_;

    // Reconnection backoff
    int reconnect_attempts_ = 0;
    static constexpr int kMaxReconnectAttempts = 5;
    static constexpr int kBaseBackoffMs = 500;
    static constexpr int kMaxBackoffMs = 30000;

    // Receive buffer for framing
    std::string recv_buffer_;

    // Internal methods
    Result<void, std::string> init_ssl();
    Result<void, std::string> try_reconnect();
    int backoff_ms() const;

    // Frame protocol
    static std::string frame_message(const std::string& payload);
    bool try_extract_frame(std::string& buffer, std::string& out_frame);

    // Ed25519 key operations
    static Result<std::vector<unsigned char>, std::string> load_private_key(const std::string& path);
    static std::string base64_encode(const unsigned char* data, size_t len);
    static std::vector<unsigned char> base64_decode(const std::string& encoded);
    static Result<std::string, std::string> sign_message(EVP_PKEY* pkey,
                                                           const unsigned char* msg,
                                                           size_t msg_len);
};

} // namespace straylight
