// services/whisper/main.cpp
// Whisper daemon — encrypted inter-process messaging service for StrayLight OS.
// Exposes named channels over Unix domain sockets with X25519 + ChaCha20-Poly1305.

#include "channel.h"
#include "crypto_channel.h"

#include <straylight/config.h>
#include <straylight/daemon.h>
#include <straylight/log.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace straylight {

static constexpr const char* kSocketPath = "/run/straylight/whisper.sock";
static constexpr int kMaxClients = 128;
static constexpr int kIdleTimeoutSecs = 3600;
static constexpr int kCleanupIntervalSecs = 60;

/// Per-client state.
struct ClientState {
    int fd = -1;
    uid_t uid = 0;
    gid_t gid = 0;
    CryptoChannel crypto;
    bool authenticated = false;
    std::string current_channel;
};

class WhisperDaemon : public DaemonBase {
public:
    Result<void, SLError> init(const Config& cfg) override {
        idle_timeout_ = cfg.get<int>("idle_timeout", kIdleTimeoutSecs);
        cleanup_interval_ = cfg.get<int>("cleanup_interval", kCleanupIntervalSecs);
        max_queue_depth_ = cfg.get<int>("max_queue_depth", 1024);
        rotation_threshold_ = cfg.get<int>("rotation_threshold", 1000);

        // Create socket directory.
        std::error_code ec;
        fs::create_directories(fs::path(kSocketPath).parent_path(), ec);

        // Remove stale socket.
        ::unlink(kSocketPath);

        // Create Unix domain socket.
        listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (listen_fd_ < 0) {
            return Result<void, SLError>::error(
                SLError{SLErrorCode::IOError, "socket() failed: " +
                        std::string(strerror(errno))});
        }

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, kSocketPath, sizeof(addr.sun_path) - 1);

        if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr),
                   sizeof(addr)) < 0) {
            ::close(listen_fd_);
            return Result<void, SLError>::error(
                SLError{SLErrorCode::IOError, "bind() failed: " +
                        std::string(strerror(errno))});
        }

        // Allow all users to connect.
        ::chmod(kSocketPath, 0777);

        if (::listen(listen_fd_, 16) < 0) {
            ::close(listen_fd_);
            return Result<void, SLError>::error(
                SLError{SLErrorCode::IOError, "listen() failed: " +
                        std::string(strerror(errno))});
        }

        SL_INFO("whisper: listening on {}", kSocketPath);
        last_cleanup_ = std::chrono::steady_clock::now();

        return Result<void, SLError>::ok();
    }

    Result<void, SLError> tick() override {
        // Build pollfd array: listen socket + all clients.
        std::vector<struct pollfd> fds;
        fds.push_back({listen_fd_, POLLIN, 0});
        {
            std::lock_guard<std::mutex> lock(clients_mu_);
            for (const auto& [fd, _] : clients_) {
                fds.push_back({fd, POLLIN, 0});
            }
        }

        int ready = ::poll(fds.data(), fds.size(), 500); // 500ms timeout.
        if (ready < 0) {
            if (errno == EINTR) return Result<void, SLError>::ok();
            return Result<void, SLError>::error(
                SLError{SLErrorCode::IOError, "poll() failed"});
        }

        // Check for new connections.
        if (fds[0].revents & POLLIN) {
            accept_client();
        }

        // Check for client data.
        for (size_t i = 1; i < fds.size(); ++i) {
            if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
                handle_client(fds[i].fd);
            }
        }

        // Periodic idle channel cleanup.
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                           now - last_cleanup_)
                           .count();
        if (elapsed >= cleanup_interval_) {
            int removed = registry_.cleanup_idle(idle_timeout_);
            if (removed > 0) {
                SL_INFO("whisper: cleaned up {} idle channel(s)", removed);
            }
            last_cleanup_ = now;
        }

        return Result<void, SLError>::ok();
    }

    void shutdown() override {
        SL_INFO("whisper: shutting down");
        // Close all client connections.
        std::lock_guard<std::mutex> lock(clients_mu_);
        for (auto& [fd, _] : clients_) {
            ::close(fd);
        }
        clients_.clear();

        if (listen_fd_ >= 0) {
            ::close(listen_fd_);
            ::unlink(kSocketPath);
            listen_fd_ = -1;
        }
    }

private:
    int listen_fd_ = -1;
    int idle_timeout_ = kIdleTimeoutSecs;
    int cleanup_interval_ = kCleanupIntervalSecs;
    int max_queue_depth_ = 1024;
    int rotation_threshold_ = 1000;
    std::chrono::steady_clock::time_point last_cleanup_;

    ChannelRegistry registry_;
    std::mutex clients_mu_;
    std::map<int, ClientState> clients_;

    void accept_client() {
        struct sockaddr_un peer{};
        socklen_t peer_len = sizeof(peer);
        int client_fd = ::accept(listen_fd_,
                                  reinterpret_cast<struct sockaddr*>(&peer),
                                  &peer_len);
        if (client_fd < 0) return;

        // Get peer credentials via SCM_CREDENTIALS.
        struct ucred cred{};
        socklen_t cred_len = sizeof(cred);
        uid_t uid = 0;
        gid_t gid = 0;
        if (::getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED,
                         &cred, &cred_len) == 0) {
            uid = cred.uid;
            gid = cred.gid;
        }

        std::lock_guard<std::mutex> lock(clients_mu_);
        if (clients_.size() >= kMaxClients) {
            ::close(client_fd);
            SL_WARN("whisper: max clients reached, rejecting connection");
            return;
        }

        ClientState state;
        state.fd = client_fd;
        state.uid = uid;
        state.gid = gid;
        clients_[client_fd] = std::move(state);

        SL_DEBUG("whisper: client connected fd={} uid={}", client_fd, uid);
    }

    /// Read a length-prefixed message from fd. Returns empty on EOF/error.
    std::string read_message(int fd) {
        uint32_t len = 0;
        ssize_t n = ::recv(fd, &len, 4, MSG_WAITALL);
        if (n != 4) return {};
        if (len > 1024 * 1024) return {}; // 1MB max.

        std::string buf(len, '\0');
        n = ::recv(fd, buf.data(), len, MSG_WAITALL);
        if (n != static_cast<ssize_t>(len)) return {};
        return buf;
    }

    /// Write a length-prefixed message to fd.
    bool write_message(int fd, const std::string& msg) {
        uint32_t len = static_cast<uint32_t>(msg.size());
        if (::send(fd, &len, 4, MSG_NOSIGNAL) != 4) return false;
        if (::send(fd, msg.data(), len, MSG_NOSIGNAL) !=
            static_cast<ssize_t>(len))
            return false;
        return true;
    }

    void handle_client(int fd) {
        std::string msg = read_message(fd);
        if (msg.empty()) {
            // Client disconnected.
            disconnect_client(fd);
            return;
        }

        // Protocol: first word is the command, rest is payload.
        std::istringstream iss(msg);
        std::string cmd;
        iss >> cmd;

        std::string response;

        if (cmd == "CREATE") {
            std::string channel_name;
            iss >> channel_name;
            auto res = registry_.create(channel_name,
                                         static_cast<size_t>(max_queue_depth_));
            if (res.has_value()) {
                response = "OK created " + channel_name;
                SL_INFO("whisper: channel '{}' created by uid={}",
                        channel_name, get_client_uid(fd));
            } else {
                response = "ERR " + res.error();
            }
        } else if (cmd == "SEND") {
            std::string channel_name;
            iss >> channel_name;
            // Rest of the line is the message body.
            std::string body;
            std::getline(iss, body);
            if (!body.empty() && body[0] == ' ') body.erase(0, 1);

            auto ch_res = registry_.get(channel_name);
            if (!ch_res.has_value()) {
                response = "ERR " + ch_res.error();
            } else {
                Channel* ch = ch_res.value();
                uid_t uid = get_client_uid(fd);
                gid_t gid = get_client_gid(fd);
                if (!ch->is_permitted(uid, gid)) {
                    response = "ERR permission denied";
                } else {
                    // Encrypt the message.
                    auto& client = get_client(fd);
                    auto enc = client.crypto.encrypt(body);
                    if (!enc.has_value()) {
                        // If crypto not set up, store plaintext wrapped.
                        WhisperMessage wm;
                        wm.ciphertext.assign(
                            reinterpret_cast<const uint8_t*>(body.data()),
                            reinterpret_cast<const uint8_t*>(body.data()) +
                                body.size());
                        wm.sender_uid = uid;
                        auto enq = ch->enqueue(std::move(wm));
                        response = enq.has_value() ? "OK sent" : "ERR " + enq.error();
                    } else {
                        WhisperMessage wm;
                        wm.ciphertext = enc.value().ciphertext;
                        wm.nonce.assign(enc.value().nonce.begin(),
                                        enc.value().nonce.end());
                        wm.sender_uid = uid;
                        auto enq = ch->enqueue(std::move(wm));
                        response = enq.has_value() ? "OK sent" : "ERR " + enq.error();
                    }
                }
            }
        } else if (cmd == "RECV") {
            std::string channel_name;
            iss >> channel_name;
            auto ch_res = registry_.get(channel_name);
            if (!ch_res.has_value()) {
                response = "ERR " + ch_res.error();
            } else {
                Channel* ch = ch_res.value();
                uid_t uid = get_client_uid(fd);
                gid_t gid = get_client_gid(fd);
                if (!ch->is_permitted(uid, gid)) {
                    response = "ERR permission denied";
                } else {
                    auto deq = ch->dequeue();
                    if (!deq.has_value()) {
                        response = "ERR " + deq.error();
                    } else {
                        auto& wm = deq.value();
                        // Return ciphertext as-is (client decrypts).
                        std::string payload(
                            reinterpret_cast<const char*>(wm.ciphertext.data()),
                            wm.ciphertext.size());
                        response = "OK " + payload;
                    }
                }
            }
        } else if (cmd == "LIST") {
            auto names = registry_.list();
            std::ostringstream oss;
            oss << "OK";
            for (const auto& n : names) {
                auto ch = registry_.get(n);
                size_t depth = ch.has_value() ? ch.value()->depth() : 0;
                oss << "\n  " << n << " (" << depth << " msg)";
            }
            response = oss.str();
        } else if (cmd == "DELETE") {
            std::string channel_name;
            iss >> channel_name;
            auto res = registry_.remove(channel_name);
            response = res.has_value() ? "OK deleted" : "ERR " + res.error();
        } else if (cmd == "KEYEXCHANGE") {
            // Client sends their public key (64 hex chars).
            std::string hex_key;
            iss >> hex_key;
            auto& client = get_client(fd);
            auto gen = client.crypto.generate_keypair();
            if (!gen.has_value()) {
                response = "ERR keypair generation failed";
            } else {
                // Parse peer public key from hex.
                std::array<uint8_t, 32> peer_pub{};
                for (int i = 0; i < 32 && i * 2 + 1 < static_cast<int>(hex_key.size()); ++i) {
                    auto hi = hex_key[i * 2];
                    auto lo = hex_key[i * 2 + 1];
                    auto hex_val = [](char c) -> uint8_t {
                        if (c >= '0' && c <= '9') return c - '0';
                        if (c >= 'a' && c <= 'f') return 10 + c - 'a';
                        if (c >= 'A' && c <= 'F') return 10 + c - 'A';
                        return 0;
                    };
                    peer_pub[i] = (hex_val(hi) << 4) | hex_val(lo);
                }

                auto kx = client.crypto.key_exchange(peer_pub);
                if (!kx.has_value()) {
                    response = "ERR " + kx.error();
                } else {
                    // Send our public key back as hex.
                    std::ostringstream oss;
                    oss << "OK ";
                    const auto& pk = client.crypto.public_key();
                    for (int i = 0; i < 32; ++i) {
                        char hex[3];
                        snprintf(hex, sizeof(hex), "%02x", pk[i]);
                        oss << hex;
                    }
                    response = oss.str();
                    client.authenticated = true;
                    client.crypto.set_rotation_threshold(
                        static_cast<uint64_t>(rotation_threshold_));
                }
            }
        } else {
            response = "ERR unknown command: " + cmd;
        }

        write_message(fd, response);
    }

    void disconnect_client(int fd) {
        std::lock_guard<std::mutex> lock(clients_mu_);
        auto it = clients_.find(fd);
        if (it != clients_.end()) {
            SL_DEBUG("whisper: client disconnected fd={}", fd);
            ::close(fd);
            clients_.erase(it);
        }
    }

    uid_t get_client_uid(int fd) {
        std::lock_guard<std::mutex> lock(clients_mu_);
        auto it = clients_.find(fd);
        return it != clients_.end() ? it->second.uid : 0;
    }

    gid_t get_client_gid(int fd) {
        std::lock_guard<std::mutex> lock(clients_mu_);
        auto it = clients_.find(fd);
        return it != clients_.end() ? it->second.gid : 0;
    }

    ClientState& get_client(int fd) {
        std::lock_guard<std::mutex> lock(clients_mu_);
        return clients_[fd];
    }
};

} // namespace straylight

int main(int /*argc*/, char* /*argv*/[]) {
    straylight::Log::init("straylight-whisper");

    auto cfg_result = straylight::Config::load("/etc/straylight/whisper.conf");
    if (!cfg_result.has_value()) {
        SL_ERROR("whisper: failed to load config: {}", cfg_result.error());
        return 1;
    }

    straylight::WhisperDaemon daemon;
    return daemon.run(cfg_result.value());
}
