// tools/dns/main.cpp
// CLI front-end for straylight-dns — DNS diagnostics.

#include "dns_manager.h"

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>

// ---------------------------------------------------------------------------
// Usage
// ---------------------------------------------------------------------------

static void print_usage() {
    std::cerr
        << "straylight-dns — DNS diagnostic tool\n"
        << "\n"
        << "Usage:\n"
        << "  straylight-dns lookup <domain> [--type=A] [--server=X]    DNS lookup\n"
        << "  straylight-dns reverse <ip>                               Reverse lookup\n"
        << "  straylight-dns trace <domain>                             Trace delegation\n"
        << "  straylight-dns config                                     Show DNS config\n"
        << "  straylight-dns set-servers <server1> [server2 ...]        Set DNS servers\n"
        << "  straylight-dns flush                                      Flush DNS cache\n"
        << "  straylight-dns cache                                      Show cache entries\n"
        << "  straylight-dns propagation <domain> [--type=A] [--expect=X]  Propagation check\n"
        << "  straylight-dns benchmark [domain]                         Benchmark servers\n";
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

static std::string pad(const std::string& s, size_t width) {
    if (s.size() >= width) return s;
    return s + std::string(width - s.size(), ' ');
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

    straylight::DnsManager mgr;

    // -----------------------------------------------------------------------
    // lookup <domain> [--type=A] [--server=X]
    // -----------------------------------------------------------------------
    if (command == "lookup") {
        if (argc < 3) {
            std::cerr << "Error: 'lookup' requires a domain\n";
            return 1;
        }
        std::string domain = argv[2];
        std::string type = get_arg(argc, argv, "--type=", 3);
        std::string server = get_arg(argc, argv, "--server=", 3);

        auto res = mgr.lookup(domain, type, server);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        const auto& r = res.value();
        std::cout << "Query:  " << r.query_name << " " << r.query_type << "\n"
                  << "Server: " << r.server << "\n"
                  << "Status: " << r.status;
        if (r.authoritative) std::cout << " (authoritative)";
        std::cout << "\n"
                  << "Time:   " << r.query_time_ms << " ms\n\n";

        if (r.records.empty()) {
            std::cout << "No records returned.\n";
        } else {
            std::cout << pad("NAME", 30) << pad("TYPE", 8)
                      << pad("TTL", 8) << "VALUE\n"
                      << std::string(80, '-') << "\n";
            for (const auto& rec : r.records) {
                std::cout << pad(rec.name, 30)
                          << pad(rec.type, 8)
                          << pad(std::to_string(rec.ttl), 8)
                          << rec.value << "\n";
            }
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // reverse <ip>
    // -----------------------------------------------------------------------
    if (command == "reverse") {
        if (argc < 3) {
            std::cerr << "Error: 'reverse' requires an IP address\n";
            return 1;
        }
        auto res = mgr.reverse(argv[2]);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        const auto& r = res.value();
        std::cout << "Reverse lookup for " << argv[2] << ":\n\n";
        if (r.records.empty()) {
            std::cout << "No PTR records found.\n";
        } else {
            for (const auto& rec : r.records) {
                std::cout << "  " << rec.value << " (TTL: " << rec.ttl << ")\n";
            }
        }
        std::cout << "\nQuery time: " << r.query_time_ms << " ms\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // trace <domain>
    // -----------------------------------------------------------------------
    if (command == "trace") {
        if (argc < 3) {
            std::cerr << "Error: 'trace' requires a domain\n";
            return 1;
        }
        auto res = mgr.trace(argv[2]);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        const auto& steps = res.value();
        std::cout << "DNS delegation trace for " << argv[2] << ":\n\n";
        int step_num = 1;
        for (const auto& s : steps) {
            std::cout << "Step " << step_num++ << ":";
            if (!s.server.empty()) std::cout << " (via " << s.server << ")";
            std::cout << "\n";
            for (const auto& rec : s.records) {
                std::cout << "  " << pad(rec.type, 8) << rec.value << "\n";
            }
            std::cout << "\n";
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // config
    // -----------------------------------------------------------------------
    if (command == "config") {
        auto res = mgr.config();
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        const auto& cfg = res.value();
        std::cout << "DNS Configuration:\n"
                  << "  Mode:         " << cfg.mode << "\n"
                  << "  Servers:     ";
        for (const auto& s : cfg.servers) std::cout << " " << s;
        std::cout << "\n";
        if (!cfg.search_domains.empty()) {
            std::cout << "  Search:      ";
            for (const auto& d : cfg.search_domains) std::cout << " " << d;
            std::cout << "\n";
        }
        std::cout << "  DNSSEC:       " << (cfg.dnssec ? "enabled" : "disabled") << "\n"
                  << "  DNS-over-TLS: " << (cfg.dns_over_tls ? "enabled" : "disabled") << "\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // set-servers <server1> [server2 ...]
    // -----------------------------------------------------------------------
    if (command == "set-servers") {
        if (argc < 3) {
            std::cerr << "Error: 'set-servers' requires at least one server\n";
            return 1;
        }
        std::vector<std::string> servers;
        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg.rfind("--", 0) != 0) servers.push_back(arg);
        }
        auto res = mgr.set_servers(servers);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "DNS servers set to:";
        for (const auto& s : servers) std::cout << " " << s;
        std::cout << "\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // flush
    // -----------------------------------------------------------------------
    if (command == "flush") {
        auto res = mgr.flush_cache();
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "DNS cache flushed.\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // cache
    // -----------------------------------------------------------------------
    if (command == "cache") {
        auto res = mgr.cache_dump();
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        const auto& entries = res.value();
        if (entries.empty()) {
            std::cout << "DNS cache is empty or not accessible.\n";
            return 0;
        }
        std::cout << pad("NAME", 40) << pad("TYPE", 8)
                  << pad("TTL", 8) << "VALUE\n"
                  << std::string(80, '-') << "\n";
        for (const auto& e : entries) {
            std::cout << pad(e.name, 40) << pad(e.type, 8)
                      << pad(std::to_string(e.remaining_ttl), 8)
                      << e.value << "\n";
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // propagation <domain> [--type=A] [--expect=X]
    // -----------------------------------------------------------------------
    if (command == "propagation") {
        if (argc < 3) {
            std::cerr << "Error: 'propagation' requires a domain\n";
            return 1;
        }
        std::string domain = argv[2];
        std::string type = get_arg(argc, argv, "--type=", 3);
        std::string expected = get_arg(argc, argv, "--expect=", 3);
        if (type.empty()) type = "A";

        std::cerr << "Checking DNS propagation for " << domain << " ...\n";
        auto res = mgr.propagation(domain, type, expected);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }

        const auto& results = res.value();
        std::cout << "\n" << pad("SERVER", 18) << pad("PROVIDER", 14)
                  << pad("RESPONSE", 40) << pad("TIME", 10);
        if (!expected.empty()) std::cout << "MATCH";
        std::cout << "\n" << std::string(90, '-') << "\n";

        int matches = 0;
        for (const auto& r : results) {
            std::cout << pad(r.server, 18)
                      << pad(r.location, 14)
                      << pad(r.value.empty() ? "(no response)" : r.value, 40);
            char time_buf[16];
            snprintf(time_buf, sizeof(time_buf), "%.1fms", r.response_time_ms);
            std::cout << pad(time_buf, 10);
            if (!expected.empty()) {
                std::cout << (r.matches ? "YES" : "NO");
                if (r.matches) matches++;
            }
            std::cout << "\n";
        }

        if (!expected.empty()) {
            std::cout << "\nPropagation: " << matches << "/" << results.size()
                      << " servers have expected value\n";
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // benchmark [domain]
    // -----------------------------------------------------------------------
    if (command == "benchmark") {
        std::string domain = (argc >= 3) ? argv[2] : "google.com";
        std::cerr << "Running DNS benchmark (this may take a few seconds) ...\n";
        auto res = mgr.benchmark(domain);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "\n" << res.value();
        return 0;
    }

    std::cerr << "Error: unknown command '" << command << "'\n";
    print_usage();
    return 1;
}
