// services/swarm/node_client.cpp
#include "node_client.h"
#include <straylight/log.h>

#include <chrono>
#include <cstring>
#include <sstream>

#ifdef __linux__
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <fcntl.h>
#include <poll.h>

namespace straylight {

NodeClient::NodeClient() = default;

NodeClient::~NodeClient() {
    // Close all pooled connections
    for (auto& [id, fd] : conn_pool_) {
        if (fd >= 0) ::close(fd);
    }
}

void NodeClient::init(NodeDiscovery& discovery) {
    discovery_ = &discovery;
    SL_INFO("swarm: node client initialized");
}

// ---------------------------------------------------------------------------
// Connection management
// ---------------------------------------------------------------------------

Result<int, std::string> NodeClient::connect_to_node(const SwarmNode& node, int timeout_ms) {
    // Check connection pool first
    {
        std::lock_guard lock(mutex_);
        auto it = conn_pool_.find(node.node_id);
        if (it != conn_pool_.end() && it->second >= 0) {
            // Quick check if the socket is still alive
            struct pollfd pfd{};
            pfd.fd = it->second;
            pfd.events = POLLOUT;
            int ret = poll(&pfd, 1, 0);
            if (ret > 0 && !(pfd.revents & (POLLERR | POLLHUP))) {
                return Result<int, std::string>::ok(it->second);
            }
            // Dead connection — remove from pool
            ::close(it->second);
            conn_pool_.erase(it);
        }
    }

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return Result<int, std::string>::error("socket creation failed: " + std::string(strerror(errno)));
    }

    // Set non-blocking for connect timeout
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(node.port);

    if (inet_pton(AF_INET, node.ip_address.c_str(), &addr.sin_addr) <= 0) {
        ::close(fd);
        return Result<int, std::string>::error("invalid IP address: " + node.ip_address);
    }

    int ret = ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) {
        ::close(fd);
        return Result<int, std::string>::error("connect failed: " + std::string(strerror(errno)));
    }

    if (ret < 0) {
        // Wait for connection with timeout
        struct pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLOUT;
        ret = poll(&pfd, 1, timeout_ms);
        if (ret <= 0) {
            ::close(fd);
            return Result<int, std::string>::error("connect timed out to " + node.ip_address + ":" + std::to_string(node.port));
        }
        // Check for connect error
        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err != 0) {
            ::close(fd);
            return Result<int, std::string>::error("connect error: " + std::string(strerror(err)));
        }
    }

    // Restore blocking mode
    fcntl(fd, F_SETFL, flags);

    // Enable TCP keepalive
    int keepalive = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));

    // Disable Nagle for lower latency
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    // NOTE: In production, TLS 1.3 handshake would happen here via OpenSSL.
    // The straylight-remote protocol uses mutual TLS with certs signed by
    // the cluster CA in /etc/straylight/pki/. For macOS development,
    // we skip TLS and use plaintext.
    SL_DEBUG("swarm: connected to {} ({}:{}) fd={}", node.hostname, node.ip_address, node.port, fd);

    // Add to pool
    {
        std::lock_guard lock(mutex_);
        conn_pool_[node.node_id] = fd;
    }

    return Result<int, std::string>::ok(fd);
}

Result<std::string, std::string> NodeClient::send_request(int fd, const std::string& request) {
    // Send the full request
    const char* data = request.data();
    size_t remaining = request.size();
    while (remaining > 0) {
        ssize_t sent = ::send(fd, data, remaining, 0);
        if (sent < 0) {
            if (errno == EINTR) continue;
            return Result<std::string, std::string>::error("send failed: " + std::string(strerror(errno)));
        }
        data += sent;
        remaining -= static_cast<size_t>(sent);
    }

    // Receive response (read until double newline or timeout)
    std::string response;
    char buf[4096];
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);

    while (std::chrono::steady_clock::now() < deadline) {
        struct pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        int ret = poll(&pfd, 1, 1000);

        if (ret < 0) {
            if (errno == EINTR) continue;
            return Result<std::string, std::string>::error("poll failed: " + std::string(strerror(errno)));
        }
        if (ret == 0) continue;  // timeout, retry

        ssize_t n = ::recv(fd, buf, sizeof(buf) - 1, 0);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return Result<std::string, std::string>::error("recv failed: " + std::string(strerror(errno)));
        }
        if (n == 0) break;  // connection closed

        buf[n] = '\0';
        response += std::string(buf, static_cast<size_t>(n));

        // Check for end of response (double newline)
        if (response.find("\n\n") != std::string::npos) break;
    }

    if (response.empty()) {
        return Result<std::string, std::string>::error("no response from peer");
    }

    return Result<std::string, std::string>::ok(std::move(response));
}

void NodeClient::close_connection(int fd) {
    if (fd >= 0) {
        ::close(fd);
        std::lock_guard lock(mutex_);
        for (auto it = conn_pool_.begin(); it != conn_pool_.end(); ++it) {
            if (it->second == fd) {
                conn_pool_.erase(it);
                break;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Request builders
// ---------------------------------------------------------------------------

std::string NodeClient::build_exec_request(const std::string& task_id,
                                            const std::string& command,
                                            const std::string& working_dir,
                                            int timeout_seconds) {
    std::ostringstream oss;
    oss << "REQUEST\n"
        << "type=exec\n"
        << "task_id=" << task_id << "\n"
        << "command=" << command << "\n"
        << "working_dir=" << working_dir << "\n"
        << "timeout=" << timeout_seconds << "\n"
        << "\n";
    return oss.str();
}

std::string NodeClient::build_cancel_request(const std::string& task_id) {
    std::ostringstream oss;
    oss << "REQUEST\n"
        << "type=cancel\n"
        << "task_id=" << task_id << "\n"
        << "\n";
    return oss.str();
}

std::string NodeClient::build_ping_request() {
    return "REQUEST\ntype=ping\n\n";
}

std::string NodeClient::build_version_request() {
    return "REQUEST\ntype=version\n\n";
}

// ---------------------------------------------------------------------------
// Response parser
// ---------------------------------------------------------------------------

std::unordered_map<std::string, std::string> NodeClient::parse_response(const std::string& data) {
    std::unordered_map<std::string, std::string> fields;
    std::istringstream iss(data);
    std::string line;

    // Skip "RESPONSE" header
    if (std::getline(iss, line) && line.find("RESPONSE") == std::string::npos) {
        // Not a valid response
        return fields;
    }

    while (std::getline(iss, line)) {
        if (line.empty()) break;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        fields[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return fields;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

Result<RemoteExecResult, std::string> NodeClient::execute_remote(
    const std::string& node_id,
    const std::string& command,
    const std::string& working_dir,
    int timeout_seconds)
{
    if (!discovery_) {
        return Result<RemoteExecResult, std::string>::error("node client not initialized");
    }

    const auto* node = discovery_->find_node(node_id);
    if (!node) {
        return Result<RemoteExecResult, std::string>::error("node not found: " + node_id);
    }

    // On the local node, execute directly
    if (node->is_self) {
        SL_INFO("swarm: executing locally: {}", command);
        RemoteExecResult result;
        std::string full_cmd = "cd " + working_dir + " && " + command + " 2>&1";
        FILE* pipe = popen(full_cmd.c_str(), "r");
        if (!pipe) {
            return Result<RemoteExecResult, std::string>::error("popen failed");
        }
        char buf[4096];
        while (fgets(buf, sizeof(buf), pipe)) {
            result.stdout_output += buf;
        }
        result.exit_code = pclose(pipe);
        // pclose returns the exit status shifted; normalize
        if (WIFEXITED(result.exit_code)) {
            result.exit_code = WEXITSTATUS(result.exit_code);
        }
        return Result<RemoteExecResult, std::string>::ok(std::move(result));
    }

    // Remote execution
    SL_INFO("swarm: executing on {} ({}): {}", node->hostname, node->ip_address, command);

    auto conn = connect_to_node(*node, 5000);
    if (!conn.has_value()) {
        return Result<RemoteExecResult, std::string>::error("connection failed: " + conn.error());
    }

    int fd = conn.value();
    std::string task_id = "exec-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::string request = build_exec_request(task_id, command, working_dir, timeout_seconds);

    auto resp = send_request(fd, request);
    if (!resp.has_value()) {
        close_connection(fd);
        return Result<RemoteExecResult, std::string>::error("request failed: " + resp.error());
    }

    auto fields = parse_response(resp.value());
    RemoteExecResult result;

    if (fields.count("status") && fields["status"] == "error") {
        return Result<RemoteExecResult, std::string>::error(
            fields.count("error") ? fields["error"] : "unknown remote error");
    }

    if (fields.count("output")) {
        result.stdout_output = fields["output"];
    }
    if (fields.count("exit_code")) {
        try { result.exit_code = std::stoi(fields["exit_code"]); } catch (...) {}
    }

    return Result<RemoteExecResult, std::string>::ok(std::move(result));
}

Result<void, std::string> NodeClient::cancel_remote_task(
    const std::string& node_id,
    const std::string& task_id)
{
    if (!discovery_) {
        return Result<void, std::string>::error("node client not initialized");
    }

    const auto* node = discovery_->find_node(node_id);
    if (!node) {
        return Result<void, std::string>::error("node not found: " + node_id);
    }

    SL_INFO("swarm: cancelling task {} on {}", task_id, node->hostname);

    auto conn = connect_to_node(*node, 5000);
    if (!conn.has_value()) {
        return Result<void, std::string>::error("connection failed: " + conn.error());
    }

    int fd = conn.value();
    std::string request = build_cancel_request(task_id);
    auto resp = send_request(fd, request);
    if (!resp.has_value()) {
        close_connection(fd);
        return Result<void, std::string>::error("cancel request failed: " + resp.error());
    }

    auto fields = parse_response(resp.value());
    if (fields.count("status") && fields["status"] == "error") {
        return Result<void, std::string>::error(
            fields.count("error") ? fields["error"] : "unknown remote error");
    }

    return Result<void, std::string>::ok();
}

Result<double, std::string> NodeClient::ping_node(const std::string& node_id) {
    if (!discovery_) {
        return Result<double, std::string>::error("node client not initialized");
    }

    const auto* node = discovery_->find_node(node_id);
    if (!node) {
        return Result<double, std::string>::error("node not found: " + node_id);
    }

    auto start = std::chrono::steady_clock::now();
    auto conn = connect_to_node(*node, 5000);
    if (!conn.has_value()) {
        return Result<double, std::string>::error("connection failed: " + conn.error());
    }

    int fd = conn.value();
    std::string request = build_ping_request();
    auto resp = send_request(fd, request);

    auto end = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(end - start).count();

    if (!resp.has_value()) {
        close_connection(fd);
        return Result<double, std::string>::error("ping failed: " + resp.error());
    }

    return Result<double, std::string>::ok(ms);
}

Result<std::string, std::string> NodeClient::remote_version(const std::string& node_id) {
    if (!discovery_) {
        return Result<std::string, std::string>::error("node client not initialized");
    }

    const auto* node = discovery_->find_node(node_id);
    if (!node) {
        return Result<std::string, std::string>::error("node not found: " + node_id);
    }

    auto conn = connect_to_node(*node, 5000);
    if (!conn.has_value()) {
        return Result<std::string, std::string>::error("connection failed: " + conn.error());
    }

    int fd = conn.value();
    std::string request = build_version_request();
    auto resp = send_request(fd, request);
    if (!resp.has_value()) {
        close_connection(fd);
        return Result<std::string, std::string>::error("version request failed: " + resp.error());
    }

    auto fields = parse_response(resp.value());
    if (fields.count("version")) {
        return Result<std::string, std::string>::ok(fields["version"]);
    }
    return Result<std::string, std::string>::error("no version in response");
}

} // namespace straylight
