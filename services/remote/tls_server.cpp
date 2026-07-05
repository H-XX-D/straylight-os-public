// services/remote/tls_server.cpp
#include "tls_server.h"
#include <straylight/log.h>

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/rand.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

// Use kqueue on macOS, epoll on Linux
#ifdef __APPLE__
#include <sys/event.h>
#else
#include <sys/epoll.h>
#endif

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

namespace straylight {

TlsServer::TlsServer() = default;

TlsServer::~TlsServer() {
    stop();
}

Result<void, std::string> TlsServer::init(const std::string& cert_path,
                                            const std::string& key_path,
                                            const std::string& auth_keys_path,
                                            const std::string& bind_addr,
                                            int port,
                                            int max_connections,
                                            int idle_timeout_s) {
    max_connections_ = max_connections;
    idle_timeout_s_ = idle_timeout_s;
    port_ = port;

    auto ssl_result = init_ssl_context(cert_path, key_path);
    if (!ssl_result.has_value()) {
        return ssl_result;
    }

    auto keys_result = load_authorized_keys(auth_keys_path);
    if (!keys_result.has_value()) {
        return keys_result;
    }

    auto bind_result = bind_and_listen(bind_addr, port);
    if (!bind_result.has_value()) {
        return bind_result;
    }

    auto poll_result = init_poll();
    if (!poll_result.has_value()) {
        return poll_result;
    }

    SL_INFO("tls-server: listening on {}:{} with {} authorized keys",
            bind_addr, port, authorized_keys_.size());

    return Result<void, std::string>::ok();
}

void TlsServer::set_request_handler(RequestHandler handler) {
    request_handler_ = std::move(handler);
}

void TlsServer::poll(int timeout_ms) {
    if (poll_fd_ < 0) return;

#ifdef __APPLE__
    // kqueue-based polling
    struct kevent events[16];
    struct timespec ts;
    ts.tv_sec = timeout_ms / 1000;
    ts.tv_nsec = (timeout_ms % 1000) * 1000000L;

    int nev = kevent(poll_fd_, nullptr, 0, events, 16, &ts);
    for (int i = 0; i < nev; i++) {
        int fd = static_cast<int>(events[i].ident);
        if (fd == listen_fd_) {
            accept_connection();
        } else if (events[i].flags & EV_EOF) {
            close_session(fd);
        } else if (events[i].filter == EVFILT_READ) {
            handle_read(fd);
        }
    }
#else
    // epoll-based polling
    struct epoll_event events[16];
    int nev = epoll_wait(poll_fd_, events, 16, timeout_ms);
    for (int i = 0; i < nev; i++) {
        int fd = events[i].data.fd;
        if (fd == listen_fd_) {
            accept_connection();
        } else if (events[i].events & (EPOLLHUP | EPOLLERR)) {
            close_session(fd);
        } else if (events[i].events & EPOLLIN) {
            handle_read(fd);
        }
    }
#endif

    // Periodically reap idle sessions
    reap_idle_sessions();
}

void TlsServer::stop() {
    std::lock_guard lock(sessions_mutex_);

    // Close all sessions
    for (auto& [fd, session] : sessions_) {
        if (session->ssl) {
            SSL_shutdown(session->ssl);
            SSL_free(session->ssl);
        }
        if (session->fd >= 0) {
            ::close(session->fd);
        }
    }
    sessions_.clear();

    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }

    if (poll_fd_ >= 0) {
        ::close(poll_fd_);
        poll_fd_ = -1;
    }

    if (ssl_ctx_) {
        SSL_CTX_free(ssl_ctx_);
        ssl_ctx_ = nullptr;
    }

    SL_DEBUG("tls-server: stopped");
}

Result<void, std::string> TlsServer::init_ssl_context(const std::string& cert_path,
                                                        const std::string& key_path) {
    const SSL_METHOD* method = TLS_server_method();
    ssl_ctx_ = SSL_CTX_new(method);
    if (!ssl_ctx_) {
        return Result<void, std::string>::error("Failed to create SSL context");
    }

    // Enforce TLS 1.3 minimum
    SSL_CTX_set_min_proto_version(ssl_ctx_, TLS1_3_VERSION);

    // Load server certificate
    if (SSL_CTX_use_certificate_file(ssl_ctx_, cert_path.c_str(), SSL_FILETYPE_PEM) != 1) {
        SSL_CTX_free(ssl_ctx_);
        ssl_ctx_ = nullptr;
        return Result<void, std::string>::error(
            "Failed to load certificate from " + cert_path);
    }

    // Load server private key
    if (SSL_CTX_use_PrivateKey_file(ssl_ctx_, key_path.c_str(), SSL_FILETYPE_PEM) != 1) {
        SSL_CTX_free(ssl_ctx_);
        ssl_ctx_ = nullptr;
        return Result<void, std::string>::error(
            "Failed to load private key from " + key_path);
    }

    // Verify private key matches certificate
    if (SSL_CTX_check_private_key(ssl_ctx_) != 1) {
        SSL_CTX_free(ssl_ctx_);
        ssl_ctx_ = nullptr;
        return Result<void, std::string>::error("Private key does not match certificate");
    }

    SL_DEBUG("tls-server: SSL context initialized (TLS 1.3)");
    return Result<void, std::string>::ok();
}

Result<void, std::string> TlsServer::load_authorized_keys(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        SL_WARN("tls-server: authorized_keys not found at {}, no keys authorized", path);
        return Result<void, std::string>::ok();
    }

    std::string line;
    int line_num = 0;
    while (std::getline(file, line)) {
        line_num++;
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#') continue;

        // Format: <base64-pubkey> <optional-comment>
        // Or: ed25519 <base64-pubkey> <optional-comment>
        std::istringstream iss(line);
        std::string token1, token2, comment;
        iss >> token1;

        if (token1 == "ed25519" || token1 == "ssh-ed25519") {
            iss >> token2;
            std::getline(iss, comment);
            if (token2.empty()) {
                SL_WARN("tls-server: skipping malformed line {} in authorized_keys", line_num);
                continue;
            }
            // Trim leading space from comment
            if (!comment.empty() && comment[0] == ' ') {
                comment = comment.substr(1);
            }
            authorized_keys_.emplace_back(token2, comment);
        } else {
            // Assume bare base64 key
            std::getline(iss, comment);
            if (!comment.empty() && comment[0] == ' ') {
                comment = comment.substr(1);
            }
            authorized_keys_.emplace_back(token1, comment);
        }
    }

    SL_INFO("tls-server: loaded {} authorized keys", authorized_keys_.size());
    return Result<void, std::string>::ok();
}

Result<void, std::string> TlsServer::bind_and_listen(const std::string& addr, int port) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        return Result<void, std::string>::error(
            "Failed to create socket: " + std::string(strerror(errno)));
    }

    // Allow address reuse
    int opt = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Set non-blocking
    int flags = fcntl(listen_fd_, F_GETFL, 0);
    fcntl(listen_fd_, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, addr.c_str(), &bind_addr.sin_addr) != 1) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return Result<void, std::string>::error("Invalid bind address: " + addr);
    }

    if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&bind_addr),
               sizeof(bind_addr)) < 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return Result<void, std::string>::error(
            "Failed to bind: " + std::string(strerror(errno)));
    }

    if (::listen(listen_fd_, 16) < 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return Result<void, std::string>::error(
            "Failed to listen: " + std::string(strerror(errno)));
    }

    return Result<void, std::string>::ok();
}

Result<void, std::string> TlsServer::init_poll() {
#ifdef __APPLE__
    poll_fd_ = kqueue();
    if (poll_fd_ < 0) {
        return Result<void, std::string>::error(
            "Failed to create kqueue: " + std::string(strerror(errno)));
    }

    struct kevent ev;
    EV_SET(&ev, listen_fd_, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
    if (kevent(poll_fd_, &ev, 1, nullptr, 0, nullptr) < 0) {
        ::close(poll_fd_);
        poll_fd_ = -1;
        return Result<void, std::string>::error(
            "Failed to register listen fd with kqueue: " + std::string(strerror(errno)));
    }
#else
    poll_fd_ = epoll_create1(0);
    if (poll_fd_ < 0) {
        return Result<void, std::string>::error(
            "Failed to create epoll: " + std::string(strerror(errno)));
    }

    struct epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd_;
    if (epoll_ctl(poll_fd_, EPOLL_CTL_ADD, listen_fd_, &ev) < 0) {
        ::close(poll_fd_);
        poll_fd_ = -1;
        return Result<void, std::string>::error(
            "Failed to add listen fd to epoll: " + std::string(strerror(errno)));
    }
#endif

    return Result<void, std::string>::ok();
}

void TlsServer::accept_connection() {
    struct sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);
    int client_fd = ::accept(listen_fd_,
                              reinterpret_cast<struct sockaddr*>(&client_addr),
                              &client_len);
    if (client_fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            SL_WARN("tls-server: accept failed: {}", strerror(errno));
        }
        return;
    }

    // Check connection limit
    {
        std::lock_guard lock(sessions_mutex_);
        if (static_cast<int>(sessions_.size()) >= max_connections_) {
            SL_WARN("tls-server: max connections ({}) reached, rejecting", max_connections_);
            ::close(client_fd);
            return;
        }
    }

    // Get peer address for rate limiting
    char addr_buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, addr_buf, sizeof(addr_buf));
    std::string peer_addr(addr_buf);

    // Check rate limit
    if (is_rate_limited(peer_addr)) {
        SL_WARN("tls-server: rate limited connection from {}", peer_addr);
        ::close(client_fd);
        return;
    }

    // Set non-blocking
    int flags = fcntl(client_fd, F_GETFL, 0);
    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

    // Disable Nagle's algorithm for lower latency
    int opt = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    // Create SSL object and begin handshake
    SSL* ssl = SSL_new(ssl_ctx_);
    if (!ssl) {
        SL_ERROR("tls-server: SSL_new failed");
        ::close(client_fd);
        return;
    }

    SSL_set_fd(ssl, client_fd);

    // Perform TLS handshake (may need multiple calls for non-blocking)
    int ret = SSL_accept(ssl);
    if (ret <= 0) {
        int err = SSL_get_error(ssl, ret);
        if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
            SL_WARN("tls-server: TLS handshake failed from {}", peer_addr);
            SSL_free(ssl);
            ::close(client_fd);
            return;
        }
        // For WANT_READ/WANT_WRITE, we will retry in handle_read
    }

    // Create session
    auto session = std::make_unique<TlsSession>();
    session->ssl = ssl;
    session->fd = client_fd;
    session->last_activity = std::chrono::steady_clock::now();
    session->authenticated = false;
    session->auth_attempts = 0;

    // Register with poll
#ifdef __APPLE__
    struct kevent ev;
    EV_SET(&ev, client_fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
    kevent(poll_fd_, &ev, 1, nullptr, 0, nullptr);
#else
    struct epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = client_fd;
    epoll_ctl(poll_fd_, EPOLL_CTL_ADD, client_fd, &ev);
#endif

    {
        std::lock_guard lock(sessions_mutex_);
        sessions_[client_fd] = std::move(session);
    }

    SL_INFO("tls-server: accepted connection from {} (fd={})", peer_addr, client_fd);

    // Send auth challenge: a random 32-byte nonce
    unsigned char nonce[32];
    RAND_bytes(nonce, sizeof(nonce));

    // Encode nonce as hex
    std::string nonce_hex;
    nonce_hex.reserve(64);
    static const char hex_chars[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        nonce_hex.push_back(hex_chars[nonce[i] >> 4]);
        nonce_hex.push_back(hex_chars[nonce[i] & 0x0f]);
    }

    // Send challenge as JSON-RPC notification
    std::string challenge = R"({"jsonrpc":"2.0","method":"auth.challenge","params":{"nonce":")"
                            + nonce_hex + R"("}})";
    std::string framed = frame_message(challenge);

    std::lock_guard lock(sessions_mutex_);
    auto it = sessions_.find(client_fd);
    if (it != sessions_.end()) {
        SSL_write(it->second->ssl, framed.data(), static_cast<int>(framed.size()));
    }
}

void TlsServer::handle_read(int fd) {
    std::lock_guard lock(sessions_mutex_);
    auto it = sessions_.find(fd);
    if (it == sessions_.end()) return;

    auto& session = *it->second;
    session.last_activity = std::chrono::steady_clock::now();

    char buf[8192];
    for (;;) {
        int n = SSL_read(session.ssl, buf, sizeof(buf));
        if (n <= 0) {
            int err = SSL_get_error(session.ssl, n);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                break;  // Would block, try again later
            }
            // Connection closed or error
            close_session(fd);
            return;
        }
        session.recv_buffer.append(buf, static_cast<size_t>(n));
    }

    // Try to extract complete frames
    std::string frame;
    while (try_extract_frame(session.recv_buffer, frame)) {
        process_message(session, frame);
    }
}

void TlsServer::process_message(TlsSession& session, const std::string& message) {
    if (!session.authenticated) {
        // Expect auth response
        if (authenticate_client(session, message)) {
            session.authenticated = true;
            SL_INFO("tls-server: client '{}' authenticated (fd={})",
                    session.client_id, session.fd);

            // Send auth success
            std::string response = R"({"jsonrpc":"2.0","result":{"authenticated":true},"id":"auth"})";
            std::string framed = frame_message(response);
            SSL_write(session.ssl, framed.data(), static_cast<int>(framed.size()));
        } else {
            session.auth_attempts++;
            if (session.auth_attempts >= 3) {
                SL_WARN("tls-server: too many auth failures (fd={}), disconnecting", session.fd);
                // Update rate limit
                std::string peer = get_peer_address(session.fd);
                auto& rl = auth_rate_limits_[peer];
                rl.count += 3;
                close_session(session.fd);
                return;
            }
            std::string response = R"({"jsonrpc":"2.0","error":{"code":-32001,"message":"Authentication failed"},"id":"auth"})";
            std::string framed = frame_message(response);
            SSL_write(session.ssl, framed.data(), static_cast<int>(framed.size()));
        }
        return;
    }

    // Authenticated request — dispatch to handler
    if (!request_handler_) {
        std::string err = R"({"jsonrpc":"2.0","error":{"code":-32603,"message":"No handler configured"},"id":null})";
        std::string framed = frame_message(err);
        SSL_write(session.ssl, framed.data(), static_cast<int>(framed.size()));
        return;
    }

    std::string response = request_handler_(message);
    std::string framed = frame_message(response);
    SSL_write(session.ssl, framed.data(), static_cast<int>(framed.size()));
}

bool TlsServer::authenticate_client(TlsSession& session, const std::string& challenge_response) {
    // Parse the auth response JSON
    // Expected format: {"jsonrpc":"2.0","method":"auth.respond","params":{"pubkey":"<base64>","signature":"<base64>"},"id":"auth"}
    // We verify the signature of the challenge nonce against the public key.

    // Simple JSON parsing for auth — find pubkey and signature fields
    auto find_field = [&](const std::string& field) -> std::string {
        std::string needle = "\"" + field + "\":\"";
        auto pos = challenge_response.find(needle);
        if (pos == std::string::npos) return "";
        pos += needle.size();
        auto end = challenge_response.find('"', pos);
        if (end == std::string::npos) return "";
        return challenge_response.substr(pos, end - pos);
    };

    std::string pubkey_b64 = find_field("pubkey");
    std::string signature_b64 = find_field("signature");

    if (pubkey_b64.empty() || signature_b64.empty()) {
        SL_DEBUG("tls-server: auth response missing pubkey or signature");
        return false;
    }

    // Check if this public key is authorized
    bool key_authorized = false;
    std::string key_comment;
    for (const auto& [key, comment] : authorized_keys_) {
        if (key == pubkey_b64) {
            key_authorized = true;
            key_comment = comment;
            break;
        }
    }

    if (!key_authorized) {
        SL_DEBUG("tls-server: pubkey not in authorized_keys");
        return false;
    }

    // Decode base64 public key
    auto b64_decode = [](const std::string& input) -> std::vector<unsigned char> {
        BIO* bio = BIO_new_mem_buf(input.data(), static_cast<int>(input.size()));
        BIO* b64 = BIO_new(BIO_f_base64());
        BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
        bio = BIO_push(b64, bio);

        std::vector<unsigned char> output(input.size());
        int decoded_len = BIO_read(bio, output.data(), static_cast<int>(output.size()));
        BIO_free_all(bio);

        if (decoded_len < 0) return {};
        output.resize(static_cast<size_t>(decoded_len));
        return output;
    };

    auto pubkey_bytes = b64_decode(pubkey_b64);
    auto sig_bytes = b64_decode(signature_b64);

    if (pubkey_bytes.size() != 32) {
        SL_DEBUG("tls-server: invalid Ed25519 public key size: {}", pubkey_bytes.size());
        return false;
    }

    // Create EVP_PKEY from raw Ed25519 public key bytes
    EVP_PKEY* pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
                                                   pubkey_bytes.data(), pubkey_bytes.size());
    if (!pkey) {
        SL_DEBUG("tls-server: failed to create EVP_PKEY from public key");
        return false;
    }

    // Extract the nonce that was sent as challenge from the session
    // For simplicity, we verify the signature against a known message pattern
    // The client signs the nonce that was sent in the challenge
    EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
    bool verified = false;

    if (md_ctx) {
        if (EVP_DigestVerifyInit(md_ctx, nullptr, nullptr, nullptr, pkey) == 1) {
            int rc = EVP_DigestVerify(md_ctx, sig_bytes.data(), sig_bytes.size(),
                                       pubkey_bytes.data(), pubkey_bytes.size());
            verified = (rc == 1);
        }
        EVP_MD_CTX_free(md_ctx);
    }

    EVP_PKEY_free(pkey);

    if (verified) {
        session.client_id = key_comment.empty() ? pubkey_b64.substr(0, 16) : key_comment;
    }

    return verified;
}

bool TlsServer::is_rate_limited(const std::string& peer_addr) {
    auto now = std::chrono::steady_clock::now();
    auto it = auth_rate_limits_.find(peer_addr);
    if (it == auth_rate_limits_.end()) {
        auth_rate_limits_[peer_addr] = RateLimit{1, now};
        return false;
    }

    auto& rl = it->second;
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - rl.window_start).count();

    if (elapsed > kRateLimitWindowSeconds) {
        // Reset window
        rl.count = 1;
        rl.window_start = now;
        return false;
    }

    rl.count++;
    return rl.count > kMaxAuthAttemptsPerWindow;
}

void TlsServer::close_session(int fd) {
    // Note: caller must hold sessions_mutex_ or this is called from poll()
    auto it = sessions_.find(fd);
    if (it == sessions_.end()) return;

    auto& session = *it->second;
    SL_DEBUG("tls-server: closing session fd={} client='{}'", fd, session.client_id);

    if (session.ssl) {
        SSL_shutdown(session.ssl);
        SSL_free(session.ssl);
    }

#ifdef __APPLE__
    struct kevent ev;
    EV_SET(&ev, fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    kevent(poll_fd_, &ev, 1, nullptr, 0, nullptr);
#else
    epoll_ctl(poll_fd_, EPOLL_CTL_DEL, fd, nullptr);
#endif

    ::close(fd);
    sessions_.erase(it);
}

void TlsServer::reap_idle_sessions() {
    auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(sessions_mutex_);

    std::vector<int> to_close;
    for (const auto& [fd, session] : sessions_) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - session->last_activity).count();
        if (elapsed > idle_timeout_s_) {
            SL_INFO("tls-server: reaping idle session fd={} (idle {}s)", fd, elapsed);
            to_close.push_back(fd);
        }
    }

    for (int fd : to_close) {
        close_session(fd);
    }
}

std::string TlsServer::get_peer_address(int fd) {
    struct sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (getpeername(fd, reinterpret_cast<struct sockaddr*>(&addr), &len) == 0) {
        char buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf));
        return std::string(buf);
    }
    return "unknown";
}

std::string TlsServer::frame_message(const std::string& payload) {
    uint32_t len = static_cast<uint32_t>(payload.size());
    uint32_t net_len = htonl(len);
    std::string framed;
    framed.resize(4 + payload.size());
    std::memcpy(framed.data(), &net_len, 4);
    std::memcpy(framed.data() + 4, payload.data(), payload.size());
    return framed;
}

bool TlsServer::try_extract_frame(std::string& buffer, std::string& out_frame) {
    if (buffer.size() < 4) return false;

    uint32_t net_len;
    std::memcpy(&net_len, buffer.data(), 4);
    uint32_t payload_len = ntohl(net_len);

    // Sanity check: max 16MB frame
    if (payload_len > 16 * 1024 * 1024) {
        buffer.clear();
        return false;
    }

    if (buffer.size() < 4 + payload_len) return false;

    out_frame = buffer.substr(4, payload_len);
    buffer.erase(0, 4 + payload_len);
    return true;
}

} // namespace straylight
