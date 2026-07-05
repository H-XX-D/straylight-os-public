// tools/dns/dns_manager.h
// DNS diagnostic tool for StrayLight OS.
#pragma once

#include <straylight/result.h>

#include <cstdint>
#include <string>
#include <vector>

namespace straylight {

/// A DNS record from a lookup.
struct DnsQueryRecord {
    std::string name;
    std::string type;
    std::string value;
    int         ttl = 0;
    std::string section;  // "answer", "authority", "additional"
};

/// Full DNS query result.
struct DnsQueryResult {
    std::string query_name;
    std::string query_type;
    std::string server;
    double      query_time_ms = 0;
    std::string status;         // "NOERROR", "NXDOMAIN", etc.
    bool        authoritative = false;
    bool        recursion_available = false;
    std::vector<DnsQueryRecord> records;
};

/// DNS server configuration.
struct DnsServerConfig {
    std::string mode;  // "systemd-resolved", "resolvconf", "manual"
    std::vector<std::string> servers;
    std::vector<std::string> search_domains;
    bool dnssec = false;
    bool dns_over_tls = false;
};

/// DNS propagation check result.
struct PropagationResult {
    std::string server;
    std::string location;
    std::string value;
    double      response_time_ms = 0;
    bool        matches = false;
};

/// DNS cache entry.
struct CacheEntry {
    std::string name;
    std::string type;
    std::string value;
    int         remaining_ttl = 0;
};

class DnsManager {
public:
    DnsManager();
    ~DnsManager();

    Result<DnsQueryResult, std::string> lookup(const std::string& domain,
                                                const std::string& type,
                                                const std::string& server) const;
    Result<DnsQueryResult, std::string> reverse(const std::string& ip) const;
    Result<std::vector<DnsQueryResult>, std::string> trace(const std::string& domain) const;
    Result<DnsServerConfig, std::string> config() const;
    Result<void, std::string> set_servers(const std::vector<std::string>& servers) const;
    Result<void, std::string> flush_cache() const;
    Result<std::vector<CacheEntry>, std::string> cache_dump() const;
    Result<std::vector<PropagationResult>, std::string> propagation(
        const std::string& domain, const std::string& type,
        const std::string& expected) const;
    Result<std::string, std::string> benchmark(const std::string& domain) const;

private:
    std::string run_cmd(const std::string& cmd) const;
    DnsQueryResult parse_dig_output(const std::string& output) const;
    std::vector<std::string> public_dns_servers() const;
};

} // namespace straylight
