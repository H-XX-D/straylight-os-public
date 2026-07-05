// tools/firewall/firewall_manager.h
// nftables/iptables firewall manager for StrayLight OS.
#pragma once

#include <straylight/result.h>

#include <cstdint>
#include <string>
#include <vector>

namespace straylight {

/// A single firewall rule.
struct FwRule {
    uint32_t    id = 0;
    std::string chain;       // "input", "output", "forward"
    std::string action;      // "accept", "drop", "reject"
    std::string protocol;    // "tcp", "udp", "icmp", ""
    int         port = 0;
    std::string source;
    std::string destination;
    std::string interface;
    std::string comment;
    uint64_t    packets = 0;
    uint64_t    bytes = 0;
};

/// Firewall zone (for zone-based configuration).
struct FwZone {
    std::string name;
    std::string policy;  // "accept", "drop", "reject"
    std::vector<std::string> interfaces;
    std::vector<std::string> services;
    std::vector<int> ports;
};

/// Firewall status overview.
struct FwStatus {
    bool        active = false;
    std::string backend;  // "nftables", "iptables"
    std::string default_input;
    std::string default_output;
    std::string default_forward;
    int         total_rules = 0;
    std::vector<FwZone> zones;
};

/// Port forwarding rule.
struct FwForward {
    uint32_t    id = 0;
    std::string protocol;
    int         external_port = 0;
    std::string internal_ip;
    int         internal_port = 0;
    std::string interface;
};

/// Connection tracking entry.
struct ConnTrack {
    std::string protocol;
    std::string state;
    std::string source;
    int         src_port = 0;
    std::string destination;
    int         dst_port = 0;
    uint64_t    packets = 0;
    uint64_t    bytes = 0;
    int         timeout = 0;
};

class FirewallManager {
public:
    FirewallManager();
    ~FirewallManager();

    Result<FwStatus, std::string> status() const;
    Result<std::vector<FwRule>, std::string> list_rules() const;
    Result<void, std::string> add_rule(const FwRule& rule);
    Result<void, std::string> remove_rule(uint32_t id);
    Result<void, std::string> set_default(const std::string& chain, const std::string& policy);
    Result<void, std::string> enable();
    Result<void, std::string> disable();
    Result<void, std::string> reset();
    Result<void, std::string> add_forward(const FwForward& fwd);
    Result<std::vector<FwForward>, std::string> list_forwards() const;
    Result<void, std::string> remove_forward(uint32_t id);
    Result<std::vector<ConnTrack>, std::string> conntrack() const;
    Result<std::string, std::string> export_rules() const;
    Result<void, std::string> import_rules(const std::string& rules_file) const;

private:
    std::string run_cmd(const std::string& cmd) const;
    std::string detect_backend() const;
    Result<void, std::string> nft_add_rule(const FwRule& rule) const;
    Result<void, std::string> ipt_add_rule(const FwRule& rule) const;
    std::vector<FwRule> parse_nft_list(const std::string& output) const;
    std::vector<FwRule> parse_ipt_list(const std::string& output) const;
    std::string chain_name(const std::string& chain) const;
};

} // namespace straylight
