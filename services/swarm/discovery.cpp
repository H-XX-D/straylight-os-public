// services/swarm/discovery.cpp
#include "discovery.h"
#include <straylight/log.h>

#include <algorithm>
#include <cstring>
#include <sstream>

// Platform-specific socket headers
#ifdef __linux__
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <fcntl.h>

namespace straylight {

// ---------------------------------------------------------------------------
// HeartbeatPacket serialization
// ---------------------------------------------------------------------------

std::string HeartbeatPacket::serialize() const {
    std::ostringstream oss;
    oss << "STRAYLIGHT-HB\n"
        << "node_id=" << node.node_id << "\n"
        << "hostname=" << node.hostname << "\n"
        << "ip=" << node.ip_address << "\n"
        << "port=" << node.port << "\n"
        << "gpus=" << node.gpu_count << "\n"
        << "vram_total=" << node.vram_total << "\n"
        << "vram_free=" << node.vram_free << "\n"
        << "cpu_cores=" << node.cpu_cores << "\n"
        << "mem_total=" << node.mem_total << "\n"
        << "mem_free=" << node.mem_free << "\n"
        << "load=" << node.load_1m << "\n";
    return oss.str();
}

Result<HeartbeatPacket, std::string> HeartbeatPacket::deserialize(const std::string& data) {
    HeartbeatPacket pkt;

    std::istringstream iss(data);
    std::string line;

    // Verify magic header
    if (!std::getline(iss, line) || line != "STRAYLIGHT-HB") {
        return Result<HeartbeatPacket, std::string>::error("invalid heartbeat header");
    }

    while (std::getline(iss, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        if      (key == "node_id")    pkt.node.node_id     = val;
        else if (key == "hostname")   pkt.node.hostname     = val;
        else if (key == "ip")         pkt.node.ip_address   = val;
        else if (key == "port")       { try { pkt.node.port = static_cast<uint16_t>(std::stoi(val)); } catch (...) {} }
        else if (key == "gpus")       { try { pkt.node.gpu_count = std::stoi(val); } catch (...) {} }
        else if (key == "vram_total") { try { pkt.node.vram_total = std::stoull(val); } catch (...) {} }
        else if (key == "vram_free")  { try { pkt.node.vram_free  = std::stoull(val); } catch (...) {} }
        else if (key == "cpu_cores")  { try { pkt.node.cpu_cores  = std::stoi(val); } catch (...) {} }
        else if (key == "mem_total")  { try { pkt.node.mem_total  = std::stoull(val); } catch (...) {} }
        else if (key == "mem_free")   { try { pkt.node.mem_free   = std::stoull(val); } catch (...) {} }
        else if (key == "load")       { try { pkt.node.load_1m    = std::stod(val); } catch (...) {} }
    }

    if (pkt.node.node_id.empty()) {
        return Result<HeartbeatPacket, std::string>::error("heartbeat missing node_id");
    }

    return Result<HeartbeatPacket, std::string>::ok(std::move(pkt));
}

// ---------------------------------------------------------------------------
// NodeDiscovery
// ---------------------------------------------------------------------------

NodeDiscovery::NodeDiscovery() = default;

NodeDiscovery::~NodeDiscovery() {
    stop();
}

Result<void, std::string> NodeDiscovery::start(const SwarmNode& self) {
    self_ = self;
    self_.is_self = true;
    self_.last_seen = std::chrono::steady_clock::now();

    // Add self to registry
    {
        std::lock_guard lock(registry_mutex_);
        registry_[self_.node_id] = self_;
    }

    // Init multicast socket
    auto r = init_multicast_socket();
    if (!r.has_value()) {
        SL_WARN("swarm: multicast init failed: {} (expected on macOS)", r.error());
        // Non-fatal — discovery degrades to mDNS only
    }

    // Register mDNS
    register_mdns_service();

    running_.store(true);
    SL_INFO("swarm: discovery started (node_id={}, hostname={})", self_.node_id, self_.hostname);
    return Result<void, std::string>::ok();
}

void NodeDiscovery::stop() {
    if (!running_.exchange(false)) return;

    unregister_mdns_service();

    if (mcast_fd_ >= 0) {
        ::close(mcast_fd_);
        mcast_fd_ = -1;
    }

    SL_INFO("swarm: discovery stopped");
}

Result<void, std::string> NodeDiscovery::init_multicast_socket() {
    mcast_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (mcast_fd_ < 0) {
        return Result<void, std::string>::error("failed to create multicast socket: " + std::string(strerror(errno)));
    }

    // Allow multiple processes to bind to the same port
    int reuse = 1;
    setsockopt(mcast_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
    setsockopt(mcast_fd_, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif

    // Bind to any address on the multicast port
    struct sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons(MCAST_PORT);

    if (::bind(mcast_fd_, reinterpret_cast<struct sockaddr*>(&bind_addr), sizeof(bind_addr)) < 0) {
        ::close(mcast_fd_);
        mcast_fd_ = -1;
        return Result<void, std::string>::error("bind failed: " + std::string(strerror(errno)));
    }

    // Join multicast group
    struct ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = inet_addr(MCAST_GROUP);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);

    if (setsockopt(mcast_fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        ::close(mcast_fd_);
        mcast_fd_ = -1;
        return Result<void, std::string>::error("multicast join failed: " + std::string(strerror(errno)));
    }

    // Set socket to non-blocking
    int flags = fcntl(mcast_fd_, F_GETFL, 0);
    fcntl(mcast_fd_, F_SETFL, flags | O_NONBLOCK);

    // Set TTL to 1 (link-local only)
    unsigned char ttl = 1;
    setsockopt(mcast_fd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    // Disable loopback of multicast (we add self manually)
    unsigned char loop = 0;
    setsockopt(mcast_fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

    SL_INFO("swarm: multicast socket bound to {}:{}", MCAST_GROUP, MCAST_PORT);
    return Result<void, std::string>::ok();
}

void NodeDiscovery::send_heartbeat() {
    if (mcast_fd_ < 0 || !running_.load()) return;

    // Update self's last_seen
    self_.last_seen = std::chrono::steady_clock::now();
    {
        std::lock_guard lock(registry_mutex_);
        registry_[self_.node_id] = self_;
    }

    HeartbeatPacket pkt;
    pkt.node = self_;
    std::string data = pkt.serialize();

    struct sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = inet_addr(MCAST_GROUP);
    dest.sin_port = htons(MCAST_PORT);

    ssize_t sent = ::sendto(mcast_fd_, data.data(), data.size(), 0,
                            reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest));
    if (sent < 0) {
        SL_DEBUG("swarm: heartbeat send failed: {}", strerror(errno));
    } else {
        SL_TRACE("swarm: heartbeat sent ({} bytes)", sent);
    }
}

void NodeDiscovery::receive_heartbeats() {
    if (mcast_fd_ < 0 || !running_.load()) return;

    char buf[2048];
    struct sockaddr_in sender{};
    socklen_t sender_len = sizeof(sender);

    // Drain all pending datagrams (non-blocking)
    for (;;) {
        ssize_t n = ::recvfrom(mcast_fd_, buf, sizeof(buf) - 1, 0,
                               reinterpret_cast<struct sockaddr*>(&sender), &sender_len);
        if (n <= 0) break;  // EAGAIN/EWOULDBLOCK or error

        buf[n] = '\0';
        std::string data(buf, static_cast<size_t>(n));

        auto result = HeartbeatPacket::deserialize(data);
        if (!result.has_value()) {
            SL_DEBUG("swarm: ignoring malformed heartbeat: {}", result.error());
            continue;
        }

        auto pkt = std::move(result).value();

        // Skip our own heartbeats
        if (pkt.node.node_id == self_.node_id) continue;

        // Fill in sender IP if not present in packet
        if (pkt.node.ip_address.empty()) {
            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &sender.sin_addr, ip_str, sizeof(ip_str));
            pkt.node.ip_address = ip_str;
        }

        pkt.node.last_seen = std::chrono::steady_clock::now();

        // Measure basic latency from receive time (rough, but useful for ordering)
        {
            std::lock_guard lock(registry_mutex_);
            auto it = registry_.find(pkt.node.node_id);
            if (it == registry_.end()) {
                SL_INFO("swarm: discovered new node '{}' at {}:{}",
                        pkt.node.hostname, pkt.node.ip_address, pkt.node.port);
            }
            registry_[pkt.node.node_id] = std::move(pkt.node);
        }
    }
}

void NodeDiscovery::evict_stale_nodes(std::chrono::seconds timeout) {
    auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(registry_mutex_);

    for (auto it = registry_.begin(); it != registry_.end(); ) {
        if (it->second.is_self) {
            ++it;
            continue;
        }
        auto age = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.last_seen);
        if (age > timeout) {
            SL_INFO("swarm: evicting stale node '{}' (last seen {}s ago)",
                    it->second.hostname, age.count());
            it = registry_.erase(it);
        } else {
            ++it;
        }
    }
}

std::vector<SwarmNode> NodeDiscovery::nodes() const {
    std::lock_guard lock(registry_mutex_);
    std::vector<SwarmNode> result;
    result.reserve(registry_.size());
    for (const auto& [id, node] : registry_) {
        result.push_back(node);
    }
    // Sort: self first, then by hostname
    std::sort(result.begin(), result.end(), [](const SwarmNode& a, const SwarmNode& b) {
        if (a.is_self != b.is_self) return a.is_self;
        return a.hostname < b.hostname;
    });
    return result;
}

const SwarmNode* NodeDiscovery::find_node(const std::string& node_id) const {
    std::lock_guard lock(registry_mutex_);
    auto it = registry_.find(node_id);
    return (it != registry_.end()) ? &it->second : nullptr;
}

size_t NodeDiscovery::node_count() const {
    std::lock_guard lock(registry_mutex_);
    return registry_.size();
}

void NodeDiscovery::register_mdns_service() {
    // Register _straylight._tcp via avahi-publish or systemd-resolved.
    // On macOS/non-Avahi systems this is a no-op with a log message.
    //
    // In production on StrayLight OS, we'd use the Avahi D-Bus API
    // (org.freedesktop.Avahi) to register the service properly.
    // For now, attempt the CLI tool as a fallback.

    std::string cmd = "avahi-publish-service "
                      "\"StrayLight " + self_.hostname + "\" "
                      "_straylight._tcp " +
                      std::to_string(self_.port) +
                      " \"node_id=" + self_.node_id + "\" "
                      " >/dev/null 2>&1 &";

    int ret = std::system(cmd.c_str());
    if (ret != 0) {
        SL_DEBUG("swarm: avahi-publish-service not available (expected on macOS)");
    } else {
        SL_INFO("swarm: mDNS service registered (_straylight._tcp, port {})", self_.port);
    }
}

void NodeDiscovery::unregister_mdns_service() {
    // In production: unregister via Avahi D-Bus API.
    // The background avahi-publish process will die when we exit.
    SL_DEBUG("swarm: mDNS service unregistered");
}

} // namespace straylight
