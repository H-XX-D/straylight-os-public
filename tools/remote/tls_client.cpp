// tools/remote/tls_client.cpp
#include "tls_client.h"

#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/rand.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

#include <nlohmann/json.hpp>

namespace straylight {

TlsClient::TlsClient() = default;

TlsClient::~TlsClient() {
    disconnect();
}

Result<void, std::string> TlsClient::connect(const std::string& host, int port) {
    host_ = host;
    port_ = port;

    auto ssl_result = init_ssl();
    if (!ssl_result.has_value()) {
        return ssl_result;
    }

    // Resolve hostname
    struct addrinfo hints{}, *result = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    std::string port_str = std::to_string(port);
    int gai_err = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result);
    if (gai_err != 0) {
        return Result<void, std::string>::error(
            "Failed to resolve host: " + std::string(gai_strerror(gai_err)));
    }

    fd_ = ::socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (fd_ < 0) {
        freeaddrinfo(result);
        return Result<void, std::string>::error(
            "Failed to create socket: " + std::string(strerror(errno)));
    }

    // Disable Nagle's algorithm
    int opt = 1;
    setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    // Connect with timeout
    // Set non-blocking temporarily
    int flags = fcntl(fd_, F_GETFL, 0);
    fcntl(fd_, F_SETFL, flags | O_NONBLOCK);

    int conn_err = ::connect(fd_, result->ai_addr, result->ai_addrlen);
    freeaddrinfo(result);

    if (conn_err < 0 && errno != EINPROGRESS) {
        ::close(fd_);
        fd_ = -1;
        return Result<void, std::string>::error(
            "Connection failed: " + std::string(strerror(errno)));
    }

    if (conn_err < 0) {
        // Wait for connection with timeout (10 seconds)
        struct pollfd pfd{};
        pfd.fd = fd_;
        pfd.events = POLLOUT;
        int poll_result = ::poll(&pfd, 1, 10000);
        if (poll_result <= 0) {
            ::close(fd_);
            fd_ = -1;
            return Result<void, std::string>::error("Connection timed out");
        }

        // Check for socket error
        int sock_err = 0;
        socklen_t err_len = sizeof(sock_err);
        getsockopt(fd_, SOL_SOCKET, SO_ERROR, &sock_err, &err_len);
        if (sock_err != 0) {
            ::close(fd_);
            fd_ = -1;
            return Result<void, std::string>::error(
                "Connection failed: " + std::string(strerror(sock_err)));
        }
    }

    // Restore blocking mode for TLS
    fcntl(fd_, F_SETFL, flags);

    // Create SSL connection
    ssl_ = SSL_new(ssl_ctx_);
    if (!ssl_) {
        ::close(fd_);
        fd_ = -1;
        return Result<void, std::string>::error("SSL_new failed");
    }

    SSL_set_fd(ssl_, fd_);

    // Set SNI hostname
    SSL_set_tlsext_host_name(ssl_, host.c_str());

    // Perform TLS handshake
    int ret = SSL_connect(ssl_);
    if (ret != 1) {
        unsigned long err = ERR_get_error();
        char err_buf[256];
        ERR_error_string_n(err, err_buf, sizeof(err_buf));
        SSL_free(ssl_);
        ssl_ = nullptr;
        ::close(fd_);
        fd_ = -1;
        return Result<void, std::string>::error(
            "TLS handshake failed: " + std::string(err_buf));
    }

    connected_ = true;
    reconnect_attempts_ = 0;

    return Result<void, std::string>::ok();
}

Result<void, std::string> TlsClient::authenticate(const std::string& key_path) {
    key_path_ = key_path;

    if (!connected_) {
        return Result<void, std::string>::error("Not connected");
    }

    // Wait for auth challenge from server
    auto challenge_result = receive(10000);
    if (!challenge_result.has_value()) {
        return Result<void, std::string>::error(
            "Failed to receive auth challenge: " + challenge_result.error());
    }

    // Parse challenge to get nonce
    nlohmann::json challenge;
    try {
        challenge = nlohmann::json::parse(challenge_result.value());
    } catch (...) {
        return Result<void, std::string>::error("Invalid auth challenge JSON");
    }

    std::string nonce;
    if (challenge.contains("params") && challenge["params"].contains("nonce")) {
        nonce = challenge["params"]["nonce"];
    } else {
        return Result<void, std::string>::error("Auth challenge missing nonce");
    }

    // Load private key
    auto key_result = load_private_key(key_path);
    if (!key_result.has_value()) {
        return Result<void, std::string>::error(
            "Failed to load private key: " + key_result.error());
    }

    auto& key_bytes = key_result.value();

    // Create EVP_PKEY from raw Ed25519 private key
    EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
                                                     key_bytes.data(), key_bytes.size());
    if (!pkey) {
        return Result<void, std::string>::error("Failed to load Ed25519 private key");
    }

    // Extract public key
    size_t pub_len = 32;
    unsigned char pub_bytes[32];
    EVP_PKEY_get_raw_public_key(pkey, pub_bytes, &pub_len);
    std::string pubkey_b64 = base64_encode(pub_bytes, pub_len);

    // Sign the nonce
    auto sig_result = sign_message(pkey, pub_bytes, pub_len);
    EVP_PKEY_free(pkey);

    if (!sig_result.has_value()) {
        return Result<void, std::string>::error(
            "Failed to sign challenge: " + sig_result.error());
    }

    // Send auth response
    nlohmann::json auth_response;
    auth_response["jsonrpc"] = "2.0";
    auth_response["method"] = "auth.respond";
    auth_response["params"] = {
        {"pubkey", pubkey_b64},
        {"signature", sig_result.value()}
    };
    auth_response["id"] = "auth";

    auto send_result = send(auth_response.dump());
    if (!send_result.has_value()) {
        return Result<void, std::string>::error(
            "Failed to send auth response: " + send_result.error());
    }

    // Wait for auth result
    auto result = receive(10000);
    if (!result.has_value()) {
        return Result<void, std::string>::error(
            "Failed to receive auth result: " + result.error());
    }

    nlohmann::json auth_result;
    try {
        auth_result = nlohmann::json::parse(result.value());
    } catch (...) {
        return Result<void, std::string>::error("Invalid auth result JSON");
    }

    if (auth_result.contains("error")) {
        return Result<void, std::string>::error(
            "Authentication failed: " + auth_result["error"].value("message", "unknown"));
    }

    return Result<void, std::string>::ok();
}

Result<std::string, std::string> TlsClient::request(const std::string& method,
                                                       const std::string& params_json,
                                                       int timeout_ms) {
    if (!connected_) {
        auto recon = try_reconnect();
        if (!recon.has_value()) {
            return Result<std::string, std::string>::error("Not connected: " + recon.error());
        }
    }

    // Build JSON-RPC request
    nlohmann::json req;
    req["jsonrpc"] = "2.0";
    req["method"] = method;
    req["id"] = request_id_++;

    if (!params_json.empty()) {
        try {
            req["params"] = nlohmann::json::parse(params_json);
        } catch (...) {
            return Result<std::string, std::string>::error("Invalid params JSON");
        }
    } else {
        req["params"] = nlohmann::json::object();
    }

    auto send_result = send(req.dump());
    if (!send_result.has_value()) {
        connected_ = false;
        return Result<std::string, std::string>::error(
            "Send failed: " + send_result.error());
    }

    auto recv_result = receive(timeout_ms);
    if (!recv_result.has_value()) {
        connected_ = false;
        return Result<std::string, std::string>::error(
            "Receive failed: " + recv_result.error());
    }

    return recv_result;
}

Result<void, std::string> TlsClient::send(const std::string& message) {
    if (!ssl_) {
        return Result<void, std::string>::error("Not connected");
    }

    std::string framed = frame_message(message);
    int written = SSL_write(ssl_, framed.data(), static_cast<int>(framed.size()));
    if (written <= 0) {
        return Result<void, std::string>::error("SSL_write failed");
    }

    return Result<void, std::string>::ok();
}

Result<std::string, std::string> TlsClient::receive(int timeout_ms) {
    if (!ssl_) {
        return Result<std::string, std::string>::error("Not connected");
    }

    // Check if we already have a complete frame in the buffer
    std::string frame;
    if (try_extract_frame(recv_buffer_, frame)) {
        return Result<std::string, std::string>::ok(frame);
    }

    // Read more data with timeout
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);

    while (std::chrono::steady_clock::now() < deadline) {
        struct pollfd pfd{};
        pfd.fd = fd_;
        pfd.events = POLLIN;

        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) break;

        int poll_result = ::poll(&pfd, 1, static_cast<int>(remaining));
        if (poll_result < 0) {
            return Result<std::string, std::string>::error(
                "poll failed: " + std::string(strerror(errno)));
        }
        if (poll_result == 0) {
            continue;  // Timeout, check deadline
        }

        char buf[8192];
        int n = SSL_read(ssl_, buf, sizeof(buf));
        if (n <= 0) {
            int err = SSL_get_error(ssl_, n);
            if (err == SSL_ERROR_WANT_READ) continue;
            return Result<std::string, std::string>::error("Connection closed");
        }

        recv_buffer_.append(buf, static_cast<size_t>(n));

        if (try_extract_frame(recv_buffer_, frame)) {
            return Result<std::string, std::string>::ok(frame);
        }
    }

    return Result<std::string, std::string>::error("Receive timed out");
}

void TlsClient::disconnect() {
    if (ssl_) {
        SSL_shutdown(ssl_);
        SSL_free(ssl_);
        ssl_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    if (ssl_ctx_) {
        SSL_CTX_free(ssl_ctx_);
        ssl_ctx_ = nullptr;
    }
    connected_ = false;
    recv_buffer_.clear();
}

Result<void, std::string> TlsClient::generate_keypair(const std::string& private_key_path,
                                                         const std::string& public_key_path) {
    // Generate Ed25519 key
    EVP_PKEY* pkey = nullptr;
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    if (!ctx) {
        return Result<void, std::string>::error("Failed to create key context");
    }

    if (EVP_PKEY_keygen_init(ctx) != 1) {
        EVP_PKEY_CTX_free(ctx);
        return Result<void, std::string>::error("Failed to init keygen");
    }

    if (EVP_PKEY_keygen(ctx, &pkey) != 1) {
        EVP_PKEY_CTX_free(ctx);
        return Result<void, std::string>::error("Failed to generate Ed25519 key");
    }
    EVP_PKEY_CTX_free(ctx);

    // Write private key in PEM format
    BIO* bio = BIO_new_file(private_key_path.c_str(), "w");
    if (!bio) {
        EVP_PKEY_free(pkey);
        return Result<void, std::string>::error(
            "Cannot write private key to: " + private_key_path);
    }

    if (PEM_write_bio_PrivateKey(bio, pkey, nullptr, nullptr, 0, nullptr, nullptr) != 1) {
        BIO_free(bio);
        EVP_PKEY_free(pkey);
        return Result<void, std::string>::error("Failed to write private key PEM");
    }
    BIO_free(bio);

    // Set restrictive permissions on private key
    chmod(private_key_path.c_str(), 0600);

    // Extract raw public key and write as base64
    size_t pub_len = 32;
    unsigned char pub_bytes[32];
    EVP_PKEY_get_raw_public_key(pkey, pub_bytes, &pub_len);
    EVP_PKEY_free(pkey);

    std::string pub_b64 = base64_encode(pub_bytes, pub_len);

    // Get username for comment
    char* user = getenv("USER");
    char hostname[256];
    gethostname(hostname, sizeof(hostname));
    std::string comment = std::string(user ? user : "straylight") + "@" + hostname;

    // Write public key
    std::ofstream pub_file(public_key_path);
    if (!pub_file.is_open()) {
        return Result<void, std::string>::error(
            "Cannot write public key to: " + public_key_path);
    }
    pub_file << "ed25519 " << pub_b64 << " " << comment << "\n";
    pub_file.close();

    // Set permissions
    chmod(public_key_path.c_str(), 0644);

    return Result<void, std::string>::ok();
}

Result<std::string, std::string> TlsClient::read_public_key(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return Result<std::string, std::string>::error("Cannot open: " + path);
    }

    std::string line;
    std::getline(file, line);

    // Parse: "ed25519 <base64> <comment>" or just "<base64>"
    std::istringstream iss(line);
    std::string token1, token2;
    iss >> token1 >> token2;

    if (token1 == "ed25519" || token1 == "ssh-ed25519") {
        return Result<std::string, std::string>::ok(token2);
    }

    return Result<std::string, std::string>::ok(token1);
}

// Private methods

Result<void, std::string> TlsClient::init_ssl() {
    if (ssl_ctx_) {
        SSL_CTX_free(ssl_ctx_);
    }

    const SSL_METHOD* method = TLS_client_method();
    ssl_ctx_ = SSL_CTX_new(method);
    if (!ssl_ctx_) {
        return Result<void, std::string>::error("Failed to create SSL context");
    }

    // Enforce TLS 1.3 minimum
    SSL_CTX_set_min_proto_version(ssl_ctx_, TLS1_3_VERSION);

    return Result<void, std::string>::ok();
}

Result<void, std::string> TlsClient::try_reconnect() {
    if (reconnect_attempts_ >= kMaxReconnectAttempts) {
        return Result<void, std::string>::error(
            "Max reconnection attempts (" + std::to_string(kMaxReconnectAttempts) + ") exceeded");
    }

    int delay = backoff_ms();
    std::cerr << "Reconnecting in " << delay << "ms (attempt "
              << reconnect_attempts_ + 1 << "/" << kMaxReconnectAttempts << ")...\n";

    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    reconnect_attempts_++;

    disconnect();

    auto conn_result = connect(host_, port_);
    if (!conn_result.has_value()) {
        return conn_result;
    }

    if (!key_path_.empty()) {
        auto auth_result = authenticate(key_path_);
        if (!auth_result.has_value()) {
            return Result<void, std::string>::error(
                "Re-authentication failed: " + auth_result.error());
        }
    }

    return Result<void, std::string>::ok();
}

int TlsClient::backoff_ms() const {
    int delay = kBaseBackoffMs * (1 << reconnect_attempts_);
    if (delay > kMaxBackoffMs) delay = kMaxBackoffMs;

    // Add jitter: +/- 25%
    unsigned char rand_byte;
    RAND_bytes(&rand_byte, 1);
    double jitter = (static_cast<double>(rand_byte) / 255.0 - 0.5) * 0.5;
    delay = static_cast<int>(delay * (1.0 + jitter));

    return delay;
}

std::string TlsClient::frame_message(const std::string& payload) {
    uint32_t len = static_cast<uint32_t>(payload.size());
    uint32_t net_len = htonl(len);
    std::string framed;
    framed.resize(4 + payload.size());
    std::memcpy(framed.data(), &net_len, 4);
    std::memcpy(framed.data() + 4, payload.data(), payload.size());
    return framed;
}

bool TlsClient::try_extract_frame(std::string& buffer, std::string& out_frame) {
    if (buffer.size() < 4) return false;

    uint32_t net_len;
    std::memcpy(&net_len, buffer.data(), 4);
    uint32_t payload_len = ntohl(net_len);

    if (payload_len > 16 * 1024 * 1024) {
        buffer.clear();
        return false;
    }

    if (buffer.size() < 4 + payload_len) return false;

    out_frame = buffer.substr(4, payload_len);
    buffer.erase(0, 4 + payload_len);
    return true;
}

Result<std::vector<unsigned char>, std::string> TlsClient::load_private_key(const std::string& path) {
    // Try to load as PEM first
    BIO* bio = BIO_new_file(path.c_str(), "r");
    if (!bio) {
        return Result<std::vector<unsigned char>, std::string>::error(
            "Cannot open key file: " + path);
    }

    EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);

    if (!pkey) {
        // Try as raw key file
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            return Result<std::vector<unsigned char>, std::string>::error(
                "Cannot open key file: " + path);
        }
        std::vector<unsigned char> raw_key((std::istreambuf_iterator<char>(file)),
                                             std::istreambuf_iterator<char>());
        if (raw_key.size() == 32) {
            return Result<std::vector<unsigned char>, std::string>::ok(raw_key);
        }
        return Result<std::vector<unsigned char>, std::string>::error(
            "Failed to parse private key");
    }

    // Extract raw Ed25519 key bytes
    size_t key_len = 32;
    std::vector<unsigned char> key_bytes(32);
    if (EVP_PKEY_get_raw_private_key(pkey, key_bytes.data(), &key_len) != 1) {
        EVP_PKEY_free(pkey);
        return Result<std::vector<unsigned char>, std::string>::error(
            "Failed to extract raw Ed25519 key");
    }
    EVP_PKEY_free(pkey);

    key_bytes.resize(key_len);
    return Result<std::vector<unsigned char>, std::string>::ok(key_bytes);
}

std::string TlsClient::base64_encode(const unsigned char* data, size_t len) {
    BIO* bio = BIO_new(BIO_s_mem());
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    bio = BIO_push(b64, bio);

    BIO_write(bio, data, static_cast<int>(len));
    BIO_flush(bio);

    BUF_MEM* buf_mem = nullptr;
    BIO_get_mem_ptr(bio, &buf_mem);

    std::string result(buf_mem->data, buf_mem->length);
    BIO_free_all(bio);
    return result;
}

std::vector<unsigned char> TlsClient::base64_decode(const std::string& encoded) {
    BIO* bio = BIO_new_mem_buf(encoded.data(), static_cast<int>(encoded.size()));
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    bio = BIO_push(b64, bio);

    std::vector<unsigned char> result(encoded.size());
    int decoded_len = BIO_read(bio, result.data(), static_cast<int>(result.size()));
    BIO_free_all(bio);

    if (decoded_len < 0) return {};
    result.resize(static_cast<size_t>(decoded_len));
    return result;
}

Result<std::string, std::string> TlsClient::sign_message(EVP_PKEY* pkey,
                                                            const unsigned char* msg,
                                                            size_t msg_len) {
    EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
    if (!md_ctx) {
        return Result<std::string, std::string>::error("Failed to create signing context");
    }

    if (EVP_DigestSignInit(md_ctx, nullptr, nullptr, nullptr, pkey) != 1) {
        EVP_MD_CTX_free(md_ctx);
        return Result<std::string, std::string>::error("DigestSignInit failed");
    }

    // Get signature length
    size_t sig_len = 0;
    if (EVP_DigestSign(md_ctx, nullptr, &sig_len, msg, msg_len) != 1) {
        EVP_MD_CTX_free(md_ctx);
        return Result<std::string, std::string>::error("DigestSign (length) failed");
    }

    std::vector<unsigned char> sig(sig_len);
    if (EVP_DigestSign(md_ctx, sig.data(), &sig_len, msg, msg_len) != 1) {
        EVP_MD_CTX_free(md_ctx);
        return Result<std::string, std::string>::error("DigestSign failed");
    }
    EVP_MD_CTX_free(md_ctx);

    sig.resize(sig_len);
    return Result<std::string, std::string>::ok(
        base64_encode(sig.data(), sig.size()));
}

} // namespace straylight
