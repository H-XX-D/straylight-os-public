// tools/network/main.cpp
// CLI front-end for straylight-network — unified network management,
// WiFi analysis, and probe/diagnostics.

#include "network_manager.h"

#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Usage
// ---------------------------------------------------------------------------

static void print_usage() {
    std::cerr
        << "straylight-network — unified network management & diagnostics\n"
        << "\n"
        << "Usage:\n"
        << "  -- Interface / Connection -------------------------------------------\n"
        << "  straylight-network status                                  Interface status\n"
        << "  straylight-network scan-wifi                               Scan WiFi networks\n"
        << "  straylight-network connect <ssid> [--pass=X] [--hidden]   Connect to WiFi\n"
        << "  straylight-network disconnect [interface]                  Disconnect\n"
        << "\n"
        << "  -- VPN --------------------------------------------------------------\n"
        << "  straylight-network vpn add <name> --type=wireguard|openvpn --config=FILE\n"
        << "  straylight-network vpn connect <name>\n"
        << "  straylight-network vpn disconnect <name>\n"
        << "  straylight-network vpn list\n"
        << "\n"
        << "  -- Firewall ---------------------------------------------------------\n"
        << "  straylight-network firewall allow [--port=N] [--proto=tcp] [--from=IP] [--comment=X]\n"
        << "  straylight-network firewall deny  [--port=N] [--proto=tcp] [--from=IP]\n"
        << "  straylight-network firewall list\n"
        << "  straylight-network firewall remove <rule-id>\n"
        << "\n"
        << "  -- DNS --------------------------------------------------------------\n"
        << "  straylight-network dns set <server1> [server2 ...]\n"
        << "  straylight-network dns list\n"
        << "\n"
        << "  -- Bond / Bridge ----------------------------------------------------\n"
        << "  straylight-network bond create <name> --mode=X --members=a,b [--ip=X]\n"
        << "  straylight-network bond destroy <name>\n"
        << "  straylight-network bond list\n"
        << "\n"
        << "  -- WiFi analysis ----------------------------------------------------\n"
        << "  straylight-network wifi channels                           Channel congestion\n"
        << "  straylight-network wifi signal                             Current signal quality\n"
        << "  straylight-network wifi qr <ssid>                         QR code for network\n"
        << "  straylight-network wifi saved                              Saved WiFi profiles\n"
        << "\n"
        << "  -- Probe / Diagnostics ----------------------------------------------\n"
        << "  straylight-network probe scan [subnet]                     Subnet host discovery\n"
        << "  straylight-network probe ping <host>                       Ping a host\n"
        << "  straylight-network probe ports <host> [--ports=22,80,443] Port scan\n"
        << "  straylight-network probe trace <host>                      Traceroute\n"
        << "  straylight-network probe dns <domain> [--type=A]           DNS lookup\n"
        << "  straylight-network probe bandwidth <host> [--port=5201]    Bandwidth test\n"
        << "  straylight-network probe health                            Full health check\n";
}

// ---------------------------------------------------------------------------
// Argument helpers
// ---------------------------------------------------------------------------

static std::string get_arg(int argc, char* argv[],
                            const std::string& prefix, int start = 2) {
    for (int i = start; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind(prefix, 0) == 0) return arg.substr(prefix.size());
    }
    return "";
}

static bool has_flag(int argc, char* argv[],
                     const std::string& flag, int start = 2) {
    for (int i = start; i < argc; ++i) {
        if (std::string(argv[i]) == flag) return true;
    }
    return false;
}

static std::vector<uint16_t> parse_ports(const std::string& spec) {
    std::vector<uint16_t> ports;
    std::istringstream iss(spec);
    std::string token;
    while (std::getline(iss, token, ',')) {
        auto dash = token.find('-');
        if (dash != std::string::npos) {
            try {
                auto lo = static_cast<uint16_t>(std::stoi(token.substr(0, dash)));
                auto hi = static_cast<uint16_t>(std::stoi(token.substr(dash + 1)));
                for (uint16_t p = lo; p <= hi; ++p) ports.push_back(p);
            } catch (...) {}
        } else {
            try {
                ports.push_back(static_cast<uint16_t>(std::stoi(token)));
            } catch (...) {}
        }
    }
    return ports;
}

// ---------------------------------------------------------------------------
// Display helpers
// ---------------------------------------------------------------------------

static std::string human_bytes(uint64_t bytes) {
    const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    int idx = 0;
    double val = static_cast<double>(bytes);
    while (val >= 1024.0 && idx < 4) { val /= 1024.0; ++idx; }
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f %s", val, units[idx]);
    return buf;
}

static std::string signal_bar(int dbm) {
    int quality;
    if (dbm >= -50) quality = 4;
    else if (dbm >= -60) quality = 3;
    else if (dbm >= -70) quality = 2;
    else if (dbm >= -80) quality = 1;
    else quality = 0;

    std::string bar;
    for (int i = 0; i < 4; ++i) bar += (i < quality) ? "|" : ".";
    return bar;
}

static std::string pad(const std::string& s, size_t width) {
    if (s.size() >= width) return s;
    return s + std::string(width - s.size(), ' ');
}

static std::string format_ms(double ms) {
    std::ostringstream oss;
    if (ms < 1.0)        oss << std::fixed << std::setprecision(2) << ms << "ms";
    else if (ms < 100.0) oss << std::fixed << std::setprecision(1) << ms << "ms";
    else                 oss << std::fixed << std::setprecision(0) << ms << "ms";
    return oss.str();
}

// ---------------------------------------------------------------------------
// Probe formatters (inline — mirrors ProbeDiagnostics from the old probe tool)
// ---------------------------------------------------------------------------

static void print_hosts(const std::vector<straylight::DiscoveredHost>& hosts) {
    if (hosts.empty()) { std::cout << "No hosts found.\n"; return; }

    std::cout << "\n"
              << pad("IP ADDRESS", 18) << pad("HOSTNAME", 30)
              << pad("MAC", 20)        << pad("VENDOR", 16)
              << pad("RTT", 12)        << pad("OS", 20) << "\n"
              << std::string(116, '-') << "\n";

    for (const auto& h : hosts) {
        std::cout << pad(h.ip, 18)
                  << pad(h.hostname.empty() ? "-" : h.hostname, 30)
                  << pad(h.mac.empty()    ? "-" : h.mac,    20)
                  << pad(h.vendor.empty() ? "-" : h.vendor, 16)
                  << pad(format_ms(h.rtt_ms), 12)
                  << pad(h.os_guess.empty() ? "-" : h.os_guess, 20) << "\n";
    }
    std::cout << "\n" << hosts.size() << " host(s) found\n";
}

static void print_ports(const std::string& host,
                         const std::vector<straylight::PortResult>& ports) {
    std::cout << "\nOpen ports on " << host << ":\n\n";
    if (ports.empty()) { std::cout << "No open ports found.\n"; return; }

    std::cout << pad("PORT", 10) << pad("STATE", 12)
              << pad("SERVICE", 20) << pad("RTT", 12) << "\n"
              << std::string(54, '-') << "\n";

    for (const auto& p : ports) {
        std::cout << pad(std::to_string(p.port) + "/tcp", 10)
                  << pad(p.state,   12)
                  << pad(p.service, 20)
                  << pad(format_ms(p.rtt_ms), 12) << "\n";
    }
    std::cout << "\n" << ports.size() << " open port(s)\n";
}

static void print_trace(const std::string& host,
                         const std::vector<straylight::TraceHop>& hops) {
    std::cout << "\nTraceroute to " << host
              << " (" << hops.size() << " hops max):\n\n"
              << pad("HOP", 6) << pad("IP ADDRESS", 18)
              << pad("HOSTNAME", 40) << pad("RTT", 12) << "\n"
              << std::string(76, '-') << "\n";

    for (const auto& h : hops) {
        std::cout << pad(std::to_string(h.hop), 6);
        if (h.timeout) {
            std::cout << pad("*", 18) << pad("*", 40) << pad("*", 12);
        } else {
            std::cout << pad(h.ip.empty() ? "*" : h.ip, 18)
                      << pad(h.hostname.empty() ? h.ip : h.hostname, 40)
                      << pad(format_ms(h.rtt_ms), 12);
        }
        std::cout << "\n";
    }
}

static void print_dns(const std::string& domain,
                       const std::vector<straylight::DnsRecord>& records) {
    std::cout << "\nDNS records for " << domain << ":\n\n"
              << pad("TYPE", 8) << pad("NAME", 30)
              << pad("VALUE", 50) << pad("TTL", 8) << "\n"
              << std::string(96, '-') << "\n";

    for (const auto& r : records) {
        std::cout << pad(r.type, 8)
                  << pad(r.name, 30)
                  << pad(r.value, 50)
                  << pad(r.ttl > 0 ? std::to_string(r.ttl) : "-", 8) << "\n";
    }
    std::cout << "\n" << records.size() << " record(s)\n";
}

static void print_bandwidth(const std::string& host, double mbps) {
    std::cout << "\nBandwidth test to " << host << ":\n\n"
              << std::fixed << std::setprecision(2);
    if (mbps >= 1000.0)
        std::cout << "  Throughput: " << (mbps / 1000.0) << " Gbps\n";
    else
        std::cout << "  Throughput: " << mbps << " Mbps\n";
}

static void print_health(const std::vector<straylight::HealthCheck>& checks) {
    std::cout << "\nNetwork Health Check:\n\n";
    int passed = 0;
    int total  = static_cast<int>(checks.size());
    for (const auto& c : checks) {
        std::string status = c.passed ? "[PASS]" : "[FAIL]";
        std::cout << "  " << pad(status, 8) << pad(c.name, 16) << c.detail << "\n";
        if (c.passed) ++passed;
    }
    std::cout << "\n  Score: " << passed << "/" << total;
    if (passed == total)
        std::cout << " — Network healthy";
    else if (passed >= total / 2)
        std::cout << " — Partial connectivity";
    else
        std::cout << " — Network issues detected";
    std::cout << "\n";
}

// ===========================================================================
// main
// ===========================================================================

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string command = argv[1];
    if (command == "--help" || command == "-h") {
        print_usage();
        return 0;
    }

    straylight::NetworkManager mgr;

    // -----------------------------------------------------------------------
    // status
    // -----------------------------------------------------------------------
    if (command == "status") {
        auto res = mgr.status();
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        for (const auto& iface : res.value()) {
            if (iface.type == "loopback") continue;
            std::cout << iface.name << " (" << iface.type << ") — " << iface.state << "\n";
            if (!iface.mac.empty())  std::cout << "  MAC:     " << iface.mac << "\n";
            if (!iface.ipv4.empty()) std::cout << "  IPv4:    " << iface.ipv4 << "\n";
            if (!iface.ipv6.empty()) std::cout << "  IPv6:    " << iface.ipv6 << "\n";
            if (!iface.gateway.empty()) std::cout << "  Gateway: " << iface.gateway << "\n";
            if (!iface.ssid.empty())
                std::cout << "  WiFi:    " << iface.ssid
                          << " (" << iface.signal_dbm << " dBm)\n";
            std::cout << "  Traffic: RX " << human_bytes(iface.rx_bytes)
                      << " / TX " << human_bytes(iface.tx_bytes) << "\n\n";
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // scan-wifi
    // -----------------------------------------------------------------------
    if (command == "scan-wifi") {
        auto res = mgr.scan_wifi();
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        const auto& nets = res.value();
        if (nets.empty()) { std::cout << "No WiFi networks found.\n"; return 0; }

        std::cout << std::left
                  << std::setw(6)  << "SIGNAL"
                  << std::setw(32) << "SSID"
                  << std::setw(10) << "SECURITY"
                  << std::setw(8)  << "CHAN"
                  << "BSSID\n"
                  << std::string(72, '-') << "\n";

        for (const auto& net : nets) {
            std::string sig  = signal_bar(net.signal_strength);
            std::string ssid = net.ssid.empty() ? "(hidden)" : net.ssid;
            if (net.connected) ssid += " *";

            std::cout << std::left
                      << std::setw(6)  << sig
                      << std::setw(32) << ssid
                      << std::setw(10) << net.security
                      << std::setw(8)  << net.channel
                      << net.bssid << "\n";
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // connect <ssid> [--pass=X] [--hidden]
    // -----------------------------------------------------------------------
    if (command == "connect") {
        if (argc < 3) {
            std::cerr << "Error: 'connect' requires an SSID\n";
            return 1;
        }
        std::string ssid = argv[2];
        std::string pass = get_arg(argc, argv, "--pass=", 3);
        bool hidden      = has_flag(argc, argv, "--hidden", 3);

        auto res = mgr.connect_wifi(ssid, pass, hidden);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Connected to '" << ssid << "'\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // disconnect [interface]
    // -----------------------------------------------------------------------
    if (command == "disconnect") {
        std::string iface = (argc >= 3) ? argv[2] : "";
        auto res = mgr.disconnect(iface);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Disconnected.\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // vpn <add|connect|disconnect|list>
    // -----------------------------------------------------------------------
    if (command == "vpn") {
        if (argc < 3) {
            std::cerr << "Error: 'vpn' requires a subcommand\n";
            return 1;
        }
        std::string sub = argv[2];

        if (sub == "add") {
            if (argc < 4) {
                std::cerr << "Error: 'vpn add' requires a name\n";
                return 1;
            }
            straylight::VpnConfig vpn;
            vpn.name        = argv[3];
            vpn.type        = get_arg(argc, argv, "--type=", 4);
            vpn.config_file = get_arg(argc, argv, "--config=", 4);
            vpn.server      = get_arg(argc, argv, "--server=", 4);
            std::string port_str = get_arg(argc, argv, "--port=", 4);
            if (!port_str.empty()) vpn.port = std::atoi(port_str.c_str());
            if (vpn.type.empty()) vpn.type = "wireguard";

            auto res = mgr.vpn_add(vpn);
            if (!res.has_value()) {
                std::cerr << "Error: " << res.error() << "\n";
                return 1;
            }
            std::cout << "VPN '" << vpn.name << "' (" << vpn.type << ") added.\n";
            return 0;
        }

        if (sub == "connect") {
            if (argc < 4) {
                std::cerr << "Error: 'vpn connect' requires a name\n";
                return 1;
            }
            auto res = mgr.vpn_connect(argv[3]);
            if (!res.has_value()) {
                std::cerr << "Error: " << res.error() << "\n";
                return 1;
            }
            std::cout << "VPN '" << argv[3] << "' connected.\n";
            return 0;
        }

        if (sub == "disconnect") {
            if (argc < 4) {
                std::cerr << "Error: 'vpn disconnect' requires a name\n";
                return 1;
            }
            auto res = mgr.vpn_disconnect(argv[3]);
            if (!res.has_value()) {
                std::cerr << "Error: " << res.error() << "\n";
                return 1;
            }
            std::cout << "VPN '" << argv[3] << "' disconnected.\n";
            return 0;
        }

        if (sub == "list") {
            auto res = mgr.vpn_list();
            if (!res.has_value()) {
                std::cerr << "Error: " << res.error() << "\n";
                return 1;
            }
            const auto& vpns = res.value();
            if (vpns.empty()) {
                std::cout << "No VPN configurations found.\n";
                return 0;
            }
            std::cout << std::left
                      << std::setw(20) << "NAME"
                      << std::setw(12) << "TYPE"
                      << std::setw(10) << "STATUS"
                      << "SERVER\n"
                      << std::string(52, '-') << "\n";
            for (const auto& vpn : vpns) {
                std::cout << std::left
                          << std::setw(20) << vpn.name
                          << std::setw(12) << vpn.type
                          << std::setw(10) << (vpn.connected ? "connected" : "down")
                          << vpn.server << "\n";
            }
            return 0;
        }

        std::cerr << "Error: unknown vpn subcommand '" << sub << "'\n";
        return 1;
    }

    // -----------------------------------------------------------------------
    // firewall <allow|deny|list|remove>
    // -----------------------------------------------------------------------
    if (command == "firewall") {
        if (argc < 3) {
            std::cerr << "Error: 'firewall' requires a subcommand\n";
            return 1;
        }
        std::string sub = argv[2];

        if (sub == "allow" || sub == "deny") {
            straylight::FirewallRule rule;
            rule.action = (sub == "allow") ? "accept" : "drop";
            rule.chain  = "input";

            std::string port_str = get_arg(argc, argv, "--port=", 3);
            if (!port_str.empty()) rule.port = std::atoi(port_str.c_str());
            rule.protocol    = get_arg(argc, argv, "--proto=", 3);
            rule.source      = get_arg(argc, argv, "--from=", 3);
            rule.destination = get_arg(argc, argv, "--to=", 3);
            rule.interface   = get_arg(argc, argv, "--iface=", 3);
            rule.comment     = get_arg(argc, argv, "--comment=", 3);
            if (rule.protocol.empty()) rule.protocol = "tcp";

            auto res = mgr.firewall_add(rule);
            if (!res.has_value()) {
                std::cerr << "Error: " << res.error() << "\n";
                return 1;
            }
            std::cout << "Firewall rule added: " << sub;
            if (rule.port > 0)
                std::cout << " port " << rule.port << "/" << rule.protocol;
            if (!rule.source.empty()) std::cout << " from " << rule.source;
            std::cout << "\n";
            return 0;
        }

        if (sub == "list") {
            auto res = mgr.firewall_list();
            if (!res.has_value()) {
                std::cerr << "Error: " << res.error() << "\n";
                return 1;
            }
            const auto& rules = res.value();
            if (rules.empty()) {
                std::cout << "No firewall rules configured.\n";
                return 0;
            }
            std::cout << std::left
                      << std::setw(6)  << "ID"
                      << std::setw(10) << "CHAIN"
                      << std::setw(10) << "ACTION"
                      << std::setw(8)  << "PROTO"
                      << std::setw(18) << "SOURCE"
                      << std::setw(8)  << "PORT"
                      << "COMMENT\n"
                      << std::string(70, '-') << "\n";
            for (const auto& r : rules) {
                std::cout << std::left
                          << std::setw(6)  << r.id
                          << std::setw(10) << r.chain
                          << std::setw(10) << r.action
                          << std::setw(8)  << (r.protocol.empty() ? "any" : r.protocol)
                          << std::setw(18) << (r.source.empty()   ? "*"   : r.source)
                          << std::setw(8)  << (r.port > 0         ?
                                              std::to_string(r.port) : "*")
                          << r.comment << "\n";
            }
            return 0;
        }

        if (sub == "remove") {
            if (argc < 4) {
                std::cerr << "Error: 'firewall remove' requires a rule ID\n";
                return 1;
            }
            auto res = mgr.firewall_remove(std::stoul(argv[3]));
            if (!res.has_value()) {
                std::cerr << "Error: " << res.error() << "\n";
                return 1;
            }
            std::cout << "Firewall rule " << argv[3] << " removed.\n";
            return 0;
        }

        std::cerr << "Error: unknown firewall subcommand '" << sub << "'\n";
        return 1;
    }

    // -----------------------------------------------------------------------
    // dns <set|list>
    // -----------------------------------------------------------------------
    if (command == "dns") {
        if (argc < 3) {
            std::cerr << "Error: 'dns' requires a subcommand\n";
            return 1;
        }
        std::string sub = argv[2];

        if (sub == "set") {
            if (argc < 4) {
                std::cerr << "Error: 'dns set' requires at least one server\n";
                return 1;
            }
            straylight::DnsConfig config;
            for (int i = 3; i < argc; ++i) {
                std::string arg = argv[i];
                if (arg.rfind("--", 0) != 0) config.servers.push_back(arg);
            }
            config.dnssec      = has_flag(argc, argv, "--dnssec", 3);
            config.dns_over_tls = has_flag(argc, argv, "--dot", 3);

            std::string domains = get_arg(argc, argv, "--domains=", 3);
            if (!domains.empty()) {
                size_t pos = 0;
                while (pos < domains.size()) {
                    auto next = domains.find(',', pos);
                    config.search_domains.push_back(
                        domains.substr(pos, next == std::string::npos ? next : next - pos));
                    pos = (next == std::string::npos) ? domains.size() : next + 1;
                }
            }

            auto res = mgr.dns_set(config);
            if (!res.has_value()) {
                std::cerr << "Error: " << res.error() << "\n";
                return 1;
            }
            std::cout << "DNS servers set to:";
            for (const auto& s : config.servers) std::cout << " " << s;
            std::cout << "\n";
            return 0;
        }

        if (sub == "list") {
            auto res = mgr.dns_get();
            if (!res.has_value()) {
                std::cerr << "Error: " << res.error() << "\n";
                return 1;
            }
            const auto& config = res.value();
            std::cout << "DNS Configuration (" << config.mode << "):\n"
                      << "  Servers:";
            for (const auto& s : config.servers) std::cout << " " << s;
            std::cout << "\n";
            if (!config.search_domains.empty()) {
                std::cout << "  Search:";
                for (const auto& d : config.search_domains) std::cout << " " << d;
                std::cout << "\n";
            }
            std::cout << "  DNSSEC:       " << (config.dnssec ? "yes" : "no") << "\n"
                      << "  DNS-over-TLS: " << (config.dns_over_tls ? "yes" : "no") << "\n";
            return 0;
        }

        std::cerr << "Error: unknown dns subcommand '" << sub << "'\n";
        return 1;
    }

    // -----------------------------------------------------------------------
    // bond <create|destroy|list>
    // -----------------------------------------------------------------------
    if (command == "bond") {
        if (argc < 3) {
            std::cerr << "Error: 'bond' requires a subcommand\n";
            return 1;
        }
        std::string sub = argv[2];

        if (sub == "create") {
            if (argc < 4) {
                std::cerr << "Error: 'bond create' requires a name\n";
                return 1;
            }
            straylight::BondConfig config;
            config.name      = argv[3];
            config.mode      = get_arg(argc, argv, "--mode=", 4);
            config.ip        = get_arg(argc, argv, "--ip=", 4);
            config.is_bridge = has_flag(argc, argv, "--bridge", 4);

            std::string members = get_arg(argc, argv, "--members=", 4);
            if (!members.empty()) {
                size_t pos = 0;
                while (pos < members.size()) {
                    auto next = members.find(',', pos);
                    config.members.push_back(
                        members.substr(pos,
                                       next == std::string::npos ? next : next - pos));
                    pos = (next == std::string::npos) ? members.size() : next + 1;
                }
            }

            auto res = mgr.bond_create(config);
            if (!res.has_value()) {
                std::cerr << "Error: " << res.error() << "\n";
                return 1;
            }
            std::cout << (config.is_bridge ? "Bridge" : "Bond")
                      << " '" << config.name << "' created.\n";
            return 0;
        }

        if (sub == "destroy") {
            if (argc < 4) {
                std::cerr << "Error: 'bond destroy' requires a name\n";
                return 1;
            }
            auto res = mgr.bond_destroy(argv[3]);
            if (!res.has_value()) {
                std::cerr << "Error: " << res.error() << "\n";
                return 1;
            }
            std::cout << "Bond/bridge '" << argv[3] << "' destroyed.\n";
            return 0;
        }

        if (sub == "list") {
            auto res = mgr.bond_list();
            if (!res.has_value()) {
                std::cerr << "Error: " << res.error() << "\n";
                return 1;
            }
            const auto& bonds = res.value();
            if (bonds.empty()) {
                std::cout << "No bonds or bridges configured.\n";
                return 0;
            }
            for (const auto& b : bonds) {
                std::cout << b.name
                          << " (" << (b.is_bridge ? "bridge" : "bond") << ")";
                if (!b.mode.empty()) std::cout << " mode=" << b.mode;
                std::cout << "\n  Members:";
                for (const auto& m : b.members) std::cout << " " << m;
                std::cout << "\n";
            }
            return 0;
        }

        std::cerr << "Error: unknown bond subcommand '" << sub << "'\n";
        return 1;
    }

    // -----------------------------------------------------------------------
    // wifi <channels|signal|qr|saved>
    // -----------------------------------------------------------------------
    if (command == "wifi") {
        if (argc < 3) {
            std::cerr << "Error: 'wifi' requires a subcommand "
                      << "(channels|signal|qr|saved)\n";
            return 1;
        }
        std::string sub = argv[2];

        // --- wifi channels --------------------------------------------------
        if (sub == "channels") {
            auto res = mgr.wifi_channels();
            if (!res.has_value()) {
                std::cerr << "Error: " << res.error() << "\n";
                return 1;
            }
            const auto& channels = res.value();

            int best_24 = 0, best_24_score = 101;
            int best_5  = 0, best_5_score  = 101;

            std::cout << "=== 2.4 GHz Channels ===\n"
                      << std::left
                      << std::setw(6)  << "CH"
                      << std::setw(10) << "FREQ"
                      << std::setw(10) << "NETS"
                      << std::setw(12) << "AVG SIG"
                      << "INTERFERENCE\n"
                      << std::string(50, '-') << "\n";

            for (const auto& ch : channels) {
                if (ch.band != "2.4GHz") continue;
                int bars = ch.interference_score / 10;
                std::string ibar;
                for (int i = 0; i < 10; ++i) ibar += (i < bars) ? "#" : ".";

                std::cout << std::left
                          << std::setw(6)  << ch.channel
                          << std::setw(10) << ch.frequency_mhz
                          << std::setw(10) << ch.network_count
                          << std::setw(12) << (ch.avg_signal_dbm != 0
                                ? std::to_string(ch.avg_signal_dbm) + " dBm" : "-")
                          << "[" << ibar << "] " << ch.interference_score << "%\n";

                if (ch.interference_score < best_24_score) {
                    best_24_score = ch.interference_score;
                    best_24       = ch.channel;
                }
            }
            std::cout << "\nBest 2.4GHz channel: " << best_24 << "\n\n";

            std::cout << "=== 5 GHz Channels ===\n"
                      << std::left
                      << std::setw(6)  << "CH"
                      << std::setw(10) << "FREQ"
                      << std::setw(10) << "NETS"
                      << std::setw(12) << "AVG SIG"
                      << "INTERFERENCE\n"
                      << std::string(50, '-') << "\n";

            for (const auto& ch : channels) {
                if (ch.band != "5GHz") continue;
                if (ch.network_count == 0 && ch.interference_score == 0) continue;

                int bars = ch.interference_score / 10;
                std::string ibar;
                for (int i = 0; i < 10; ++i) ibar += (i < bars) ? "#" : ".";

                std::cout << std::left
                          << std::setw(6)  << ch.channel
                          << std::setw(10) << ch.frequency_mhz
                          << std::setw(10) << ch.network_count
                          << std::setw(12) << (ch.avg_signal_dbm != 0
                                ? std::to_string(ch.avg_signal_dbm) + " dBm" : "-")
                          << "[" << ibar << "] " << ch.interference_score << "%\n";

                if (ch.interference_score < best_5_score) {
                    best_5_score = ch.interference_score;
                    best_5       = ch.channel;
                }
            }
            if (best_5 > 0)
                std::cout << "\nBest 5GHz channel: " << best_5 << "\n";
            return 0;
        }

        // --- wifi signal ----------------------------------------------------
        if (sub == "signal") {
            auto res = mgr.wifi_signal();
            if (!res.has_value()) {
                std::cerr << "Error: " << res.error() << "\n";
                return 1;
            }
            const auto& sq = res.value();
            std::cout << "WiFi Signal Quality\n"
                      << "  Network:      " << sq.ssid << "\n"
                      << "  Signal:       " << sq.signal_dbm << " dBm "
                      << signal_bar(sq.signal_dbm) << "\n"
                      << "  Link Quality: " << sq.link_quality << "%\n"
                      << "  Channel:      " << sq.channel
                      << " (" << sq.frequency_mhz << " MHz)\n";
            if (sq.noise_dbm != 0) {
                std::cout << "  Noise:        " << sq.noise_dbm << " dBm\n"
                          << "  SNR:          " << (sq.signal_dbm - sq.noise_dbm) << " dB\n";
            }
            if (sq.tx_rate_mbps > 0)
                std::cout << "  TX Rate:      " << sq.tx_rate_mbps << " Mbit/s\n";
            if (sq.rx_rate_mbps > 0)
                std::cout << "  RX Rate:      " << sq.rx_rate_mbps << " Mbit/s\n";
            return 0;
        }

        // --- wifi qr <ssid> -------------------------------------------------
        if (sub == "qr") {
            if (argc < 4) {
                std::cerr << "Error: 'wifi qr' requires an SSID\n";
                return 1;
            }
            auto res = mgr.wifi_qr(argv[3]);
            if (!res.has_value()) {
                std::cerr << "Error: " << res.error() << "\n";
                return 1;
            }
            std::cout << res.value();
            return 0;
        }

        // --- wifi saved -----------------------------------------------------
        if (sub == "saved") {
            auto res = mgr.saved_wifi();
            if (!res.has_value()) {
                std::cerr << "Error: " << res.error() << "\n";
                return 1;
            }
            const auto& nets = res.value();
            if (nets.empty()) {
                std::cout << "No saved WiFi networks.\n";
                return 0;
            }
            std::cout << std::left
                      << std::setw(28) << "SSID"
                      << std::setw(12) << "SECURITY"
                      << std::setw(8)  << "AUTO"
                      << "LAST CONNECTED\n"
                      << std::string(68, '-') << "\n";
            for (const auto& net : nets) {
                std::cout << std::left
                          << std::setw(28) << net.ssid
                          << std::setw(12) << net.security
                          << std::setw(8)  << (net.auto_connect ? "yes" : "no")
                          << net.last_connected << "\n";
            }
            return 0;
        }

        std::cerr << "Error: unknown wifi subcommand '" << sub << "'\n";
        return 1;
    }

    // -----------------------------------------------------------------------
    // probe <scan|ping|ports|trace|dns|bandwidth|health>
    // -----------------------------------------------------------------------
    if (command == "probe") {
        if (argc < 3) {
            std::cerr << "Error: 'probe' requires a subcommand "
                      << "(scan|ping|ports|trace|dns|bandwidth|health)\n";
            return 1;
        }
        std::string sub = argv[2];

        // --- probe scan [subnet] --------------------------------------------
        if (sub == "scan") {
            std::string subnet;
            if (argc >= 4 && argv[3][0] != '-') {
                subnet = argv[3];
            } else {
                auto det = mgr.detect_subnet();
                if (!det.has_value()) {
                    std::cerr << "Error: " << det.error() << "\n"
                              << "Specify a subnet: ... probe scan 192.0.2.0/24\n";
                    return 1;
                }
                subnet = det.value();
                std::cerr << "Auto-detected subnet: " << subnet << "\n";
            }

            std::cerr << "Scanning " << subnet << " ...\n";
            auto r = mgr.scan_subnet(subnet);
            if (!r.has_value()) {
                std::cerr << "Error: " << r.error() << "\n";
                return 1;
            }
            print_hosts(r.value());
            return 0;
        }

        // --- probe ping <host> ----------------------------------------------
        if (sub == "ping") {
            if (argc < 4) {
                std::cerr << "Usage: straylight-network probe ping <host>\n";
                return 1;
            }
            auto r = mgr.ping_host(argv[3]);
            if (!r.has_value()) {
                std::cerr << "Error: " << r.error() << "\n";
                return 1;
            }
            const auto& h = r.value();
            std::cout << "Host: " << h.ip;
            if (!h.hostname.empty() && h.hostname != h.ip)
                std::cout << " (" << h.hostname << ")";
            std::cout << "\nRTT:  " << h.rtt_ms << " ms\n";
            if (!h.os_guess.empty())
                std::cout << "OS:   " << h.os_guess << "\n";
            return 0;
        }

        // --- probe ports <host> [--ports=22,80,443] -------------------------
        if (sub == "ports") {
            if (argc < 4) {
                std::cerr << "Usage: straylight-network probe ports <host> "
                          << "[--ports=22,80,443]\n";
                return 1;
            }
            std::string host = argv[3];
            std::vector<uint16_t> ports;
            std::string port_spec = get_arg(argc, argv, "--ports=", 4);
            if (!port_spec.empty()) ports = parse_ports(port_spec);

            std::cerr << "Scanning ports on " << host << " ...\n";
            auto r = mgr.scan_ports(host, ports);
            if (!r.has_value()) {
                std::cerr << "Error: " << r.error() << "\n";
                return 1;
            }
            print_ports(host, r.value());
            return 0;
        }

        // --- probe trace <host> ---------------------------------------------
        if (sub == "trace") {
            if (argc < 4) {
                std::cerr << "Usage: straylight-network probe trace <host>\n";
                return 1;
            }
            std::string host = argv[3];
            int max_hops = 30;
            std::string hops_str = get_arg(argc, argv, "--hops=", 4);
            if (!hops_str.empty()) {
                try { max_hops = std::stoi(hops_str); } catch (...) {}
            }

            std::cerr << "Tracing route to " << host << " ...\n";
            auto r = mgr.traceroute(host, max_hops);
            if (!r.has_value()) {
                std::cerr << "Error: " << r.error() << "\n";
                return 1;
            }
            print_trace(host, r.value());
            return 0;
        }

        // --- probe dns <domain> [--type=A] ----------------------------------
        if (sub == "dns") {
            if (argc < 4) {
                std::cerr << "Usage: straylight-network probe dns <domain> "
                          << "[--type=A]\n";
                return 1;
            }
            std::string domain = argv[3];
            std::string type   = get_arg(argc, argv, "--type=", 4);
            if (type.empty()) type = "ANY";

            auto r = mgr.dns_lookup(domain, type);
            if (!r.has_value()) {
                std::cerr << "Error: " << r.error() << "\n";
                return 1;
            }
            print_dns(domain, r.value());
            return 0;
        }

        // --- probe bandwidth <host> [--port=5201] ---------------------------
        if (sub == "bandwidth") {
            if (argc < 4) {
                std::cerr << "Usage: straylight-network probe bandwidth <host> "
                          << "[--port=5201]\n";
                return 1;
            }
            std::string host = argv[3];
            uint16_t port    = 5201;
            std::string port_str = get_arg(argc, argv, "--port=", 4);
            if (!port_str.empty()) {
                try { port = static_cast<uint16_t>(std::stoi(port_str)); } catch (...) {}
            }

            std::cerr << "Testing bandwidth to " << host << ":" << port << " ...\n";
            auto r = mgr.bandwidth_test(host, port);
            if (!r.has_value()) {
                std::cerr << "Error: " << r.error() << "\n";
                return 1;
            }
            print_bandwidth(host, r.value());
            return 0;
        }

        // --- probe health ---------------------------------------------------
        if (sub == "health") {
            std::cerr << "Running network health checks ...\n";
            auto r = mgr.health_check();
            if (!r.has_value()) {
                std::cerr << "Error: " << r.error() << "\n";
                return 1;
            }
            print_health(r.value());
            return 0;
        }

        std::cerr << "Error: unknown probe subcommand '" << sub << "'\n";
        return 1;
    }

    std::cerr << "Error: unknown command '" << command << "'\n";
    print_usage();
    return 1;
}
