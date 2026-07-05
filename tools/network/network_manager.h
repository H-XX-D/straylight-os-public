// tools/network/network_manager.h
// Unified network management for StrayLight OS.
// Merges: NetworkManager (wifi/vpn/firewall/dns/bond),
//         WifiManager (channels/signal/qr), and
//         NetworkScanner/ProbeDiagnostics (subnet scan/ping/ports/trace/dns-lookup/bandwidth/health).
#pragma once

#include <straylight/result.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace straylight {

// ===========================================================================
// Existing NetworkManager structs (unchanged)
// ===========================================================================

/// WiFi network discovered by scanning.
struct WifiNetwork {
    std::string ssid;
    std::string bssid;
    int signal_strength = 0;   // dBm or percentage
    int frequency_mhz = 0;
    std::string security;      // "WPA2", "WPA3", "WEP", "Open"
    bool connected = false;
    int channel = 0;
    double rate_mbps = 0;
};

/// Network interface status.
struct InterfaceStatus {
    std::string name;          // e.g. "eth0", "wlan0"
    std::string type;          // "ethernet", "wifi", "loopback", "bridge", "bond"
    std::string state;         // "up", "down", "dormant"
    std::string ipv4;
    std::string ipv6;
    std::string gateway;
    std::string mac;
    int mtu = 1500;
    uint64_t rx_bytes = 0;
    uint64_t tx_bytes = 0;
    std::string ssid;          // current WiFi SSID if applicable
    int signal_dbm = 0;
};

/// VPN connection configuration.
struct VpnConfig {
    std::string name;
    std::string type;          // "wireguard", "openvpn", "ipsec"
    std::string server;
    int port = 0;
    std::string config_file;
    bool connected = false;
    std::string local_ip;
    std::string public_key;    // wireguard
};

/// Firewall rule.
struct FirewallRule {
    uint32_t id = 0;
    std::string chain;         // "input", "output", "forward"
    std::string action;        // "accept", "drop", "reject"
    std::string protocol;      // "tcp", "udp", "icmp", "any"
    std::string source;        // IP or CIDR
    std::string destination;
    int port = 0;
    std::string interface;
    std::string comment;
};

/// DNS configuration.
struct DnsConfig {
    std::vector<std::string> servers;
    std::vector<std::string> search_domains;
    std::string mode;          // "systemd-resolved", "resolv.conf", "dnsmasq"
    bool dnssec = false;
    bool dns_over_tls = false;
};

/// Network bond/bridge configuration.
struct BondConfig {
    std::string name;
    std::string mode;          // "balance-rr", "active-backup", "802.3ad", etc.
    std::vector<std::string> members;
    std::string ip;
    bool is_bridge = false;
};

/// Saved network connection.
struct SavedConnection {
    std::string name;
    std::string type;          // "wifi", "vpn", "ethernet"
    std::string ssid;
    bool auto_connect = true;
    std::string last_used;
};

// ===========================================================================
// WiFi-extended structs (from WifiManager)
// ===========================================================================

/// Channel usage analysis.
struct ChannelInfo {
    int channel = 0;
    int frequency_mhz = 0;
    int network_count = 0;      // how many APs on this channel
    int avg_signal_dbm = 0;
    int interference_score = 0; // 0=clear, 100=congested
    std::string band;           // "2.4GHz" or "5GHz"
};

/// Signal quality snapshot for the active connection.
struct SignalQuality {
    std::string ssid;
    std::string bssid;
    int signal_dbm = 0;
    int noise_dbm = 0;
    int link_quality = 0;   // 0-100
    double tx_rate_mbps = 0;
    double rx_rate_mbps = 0;
    int channel = 0;
    int frequency_mhz = 0;
};

/// Saved WiFi network profile (wifi-manager style).
struct SavedWifi {
    std::string ssid;
    std::string security;
    bool auto_connect = true;
    std::string last_connected;  // ISO-8601
    int priority = 0;
};

// ===========================================================================
// Probe/scanner structs (from NetworkScanner / ProbeDiagnostics)
// ===========================================================================

/// A discovered host on the network.
struct DiscoveredHost {
    std::string ip;
    std::string mac;
    std::string vendor;      // MAC vendor from OUI lookup
    std::string hostname;    // reverse DNS if available
    int ttl = 0;             // for OS fingerprinting
    std::string os_guess;    // based on TTL
    double rtt_ms = 0.0;     // round-trip time
    bool alive = false;
};

/// An open port on a host.
struct PortResult {
    uint16_t port = 0;
    std::string state;       // "open", "closed", "filtered"
    std::string service;     // well-known service name
    double rtt_ms = 0.0;
};

/// A traceroute hop.
struct TraceHop {
    int hop = 0;
    std::string ip;
    std::string hostname;
    double rtt_ms = 0.0;     // average of probes
    bool timeout = false;
};

/// A resolved DNS record.
struct DnsRecord {
    std::string type;    // A, AAAA, CNAME, MX, NS, TXT, SOA, PTR
    std::string name;
    std::string value;
    int ttl = 0;
};

/// Network health check result.
struct HealthCheck {
    std::string name;
    bool passed = false;
    std::string detail;
    double latency_ms = 0.0;
};

// ===========================================================================
// NetworkManager — unified class
// ===========================================================================

class NetworkManager {
public:
    NetworkManager();
    ~NetworkManager();

    // -----------------------------------------------------------------------
    // Original NetworkManager API (unchanged)
    // -----------------------------------------------------------------------

    /// Get status of all network interfaces.
    Result<std::vector<InterfaceStatus>, std::string> status() const;

    /// Scan for available WiFi networks.
    Result<std::vector<WifiNetwork>, std::string> scan_wifi() const;

    /// Connect to a WiFi network.
    Result<void, std::string> connect_wifi(const std::string& ssid,
                                            const std::string& password = "",
                                            bool hidden = false);

    /// Disconnect from current network.
    Result<void, std::string> disconnect(const std::string& interface = "");

    /// Add a VPN configuration.
    Result<void, std::string> vpn_add(const VpnConfig& config);

    /// Connect to a VPN.
    Result<void, std::string> vpn_connect(const std::string& name);

    /// Disconnect from a VPN.
    Result<void, std::string> vpn_disconnect(const std::string& name);

    /// List VPN configurations.
    Result<std::vector<VpnConfig>, std::string> vpn_list() const;

    /// Add a firewall rule.
    Result<void, std::string> firewall_add(const FirewallRule& rule);

    /// Remove a firewall rule.
    Result<void, std::string> firewall_remove(uint32_t rule_id);

    /// List firewall rules.
    Result<std::vector<FirewallRule>, std::string> firewall_list() const;

    /// Set DNS configuration.
    Result<void, std::string> dns_set(const DnsConfig& config);

    /// Get current DNS configuration.
    Result<DnsConfig, std::string> dns_get() const;

    /// Create a network bond or bridge.
    Result<void, std::string> bond_create(const BondConfig& config);

    /// Destroy a bond or bridge.
    Result<void, std::string> bond_destroy(const std::string& name);

    /// List bonds and bridges.
    Result<std::vector<BondConfig>, std::string> bond_list() const;

    /// List saved connections.
    std::vector<SavedConnection> saved_connections() const;

    /// Forget a saved connection.
    Result<void, std::string> forget_connection(const std::string& name);

    // -----------------------------------------------------------------------
    // WiFi-extended API (from WifiManager)
    // -----------------------------------------------------------------------

    /// Analyze WiFi channel congestion across 2.4 GHz and 5 GHz bands.
    Result<std::vector<ChannelInfo>, std::string> wifi_channels() const;

    /// Get signal quality for the currently associated WiFi connection.
    Result<SignalQuality, std::string> wifi_signal() const;

    /// Generate a WiFi QR code string (or ASCII fallback) for an SSID.
    Result<std::string, std::string> wifi_qr(const std::string& ssid) const;

    /// List saved WiFi profiles (wifi-manager style, with timestamps).
    Result<std::vector<SavedWifi>, std::string> saved_wifi() const;

    // -----------------------------------------------------------------------
    // Probe/diagnostics API (from NetworkScanner)
    // -----------------------------------------------------------------------

    /// Auto-detect the local subnet (e.g. "192.0.2.0/24").
    Result<std::string, std::string> detect_subnet() const;

    /// ARP + TCP sweep of a CIDR subnet; returns all responding hosts.
    Result<std::vector<DiscoveredHost>, std::string> scan_subnet(
        const std::string& subnet) const;

    /// Ping a single host and return discovery info.
    Result<DiscoveredHost, std::string> ping_host(const std::string& host) const;

    /// TCP connect scan of specified (or common default) ports on a host.
    Result<std::vector<PortResult>, std::string> scan_ports(
        const std::string& host,
        const std::vector<uint16_t>& ports = {}) const;

    /// Traceroute to a host with per-hop latency.
    Result<std::vector<TraceHop>, std::string> traceroute(
        const std::string& host,
        int max_hops = 30) const;

    /// DNS resolution for the requested record type (A, AAAA, ANY …).
    Result<std::vector<DnsRecord>, std::string> dns_lookup(
        const std::string& domain,
        const std::string& type = "ANY") const;

    /// TCP bandwidth test — measures throughput to an iperf3-compatible server.
    Result<double, std::string> bandwidth_test(
        const std::string& host,
        uint16_t port = 5201) const;

    /// Full network health check (gateway, DNS, internet, packet-loss).
    Result<std::vector<HealthCheck>, std::string> health_check() const;

private:
    // -----------------------------------------------------------------------
    // Private helpers — original NetworkManager
    // -----------------------------------------------------------------------
    std::string config_dir() const;
    Result<std::string, std::string> run_cmd(const std::string& cmd) const;
    bool has_nmcli() const;
    bool has_iw() const;
    std::vector<InterfaceStatus> parse_nmcli_status(const std::string& output) const;
    std::vector<WifiNetwork>     parse_iw_scan(const std::string& output) const;
    std::vector<InterfaceStatus> parse_ip_addr(const std::string& output) const;
    InterfaceStatus              read_sysfs_interface(const std::string& name) const;

    // -----------------------------------------------------------------------
    // Private helpers — WiFi-extended (from WifiManager)
    // -----------------------------------------------------------------------
    std::string find_wifi_interface() const;
    int freq_to_channel(int freq_mhz) const;
    std::string wifi_signal_bar(int dbm) const;
    void save_wifi_profile(const std::string& ssid,
                           const std::string& security) const;

    // -----------------------------------------------------------------------
    // Private helpers — probe/scanner (from NetworkScanner)
    // -----------------------------------------------------------------------
    static std::string guess_os_from_ttl(int ttl);
    static std::string lookup_mac_vendor(const std::string& mac);
    static std::string service_name(uint16_t port);
    static std::vector<uint16_t> default_ports();
    static Result<std::pair<uint32_t, uint32_t>, std::string> parse_cidr(
        const std::string& cidr);
    static std::string ip_to_string(uint32_t ip);
    static Result<uint32_t, std::string> string_to_ip(const std::string& ip);
};

} // namespace straylight
