// tools/dns/dns_manager.cpp
// Full DNS diagnostic implementation for StrayLight OS.

#include "dns_manager.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

namespace straylight {

DnsManager::DnsManager() = default;
DnsManager::~DnsManager() = default;

std::string DnsManager::run_cmd(const std::string& cmd) const {
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return result;
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) result += buf;
    pclose(pipe);
    return result;
}

std::vector<std::string> DnsManager::public_dns_servers() const {
    return {
        "8.8.8.8",         // Google
        "1.1.1.1",         // Cloudflare
        "9.9.9.9",         // Quad9
        "208.67.222.222",  // OpenDNS
        "8.8.4.4",         // Google secondary
        "1.0.0.1",         // Cloudflare secondary
    };
}

DnsQueryResult DnsManager::parse_dig_output(const std::string& output) const {
    DnsQueryResult result;
    std::istringstream stream(output);
    std::string line;
    std::string current_section;

    while (std::getline(stream, line)) {
        // Parse header flags
        if (line.find("status: ") != std::string::npos) {
            std::regex status_re(R"(status:\s+(\w+))");
            std::smatch m;
            if (std::regex_search(line, m, status_re)) result.status = m[1].str();
            result.authoritative = (line.find(" aa") != std::string::npos);
            result.recursion_available = (line.find(" ra") != std::string::npos);
        }

        // Parse query time
        if (line.find("Query time:") != std::string::npos) {
            std::regex time_re(R"(Query time:\s+(\d+)\s+msec)");
            std::smatch m;
            if (std::regex_search(line, m, time_re))
                result.query_time_ms = std::stod(m[1].str());
        }

        // Parse SERVER
        if (line.find("SERVER:") != std::string::npos) {
            std::regex server_re(R"(SERVER:\s+(\S+))");
            std::smatch m;
            if (std::regex_search(line, m, server_re)) result.server = m[1].str();
        }

        // Section headers
        if (line.find("ANSWER SECTION") != std::string::npos) { current_section = "answer"; continue; }
        if (line.find("AUTHORITY SECTION") != std::string::npos) { current_section = "authority"; continue; }
        if (line.find("ADDITIONAL SECTION") != std::string::npos) { current_section = "additional"; continue; }
        if (line.find("QUESTION SECTION") != std::string::npos) { current_section = "question"; continue; }

        // Parse records in answer/authority/additional sections
        if (!current_section.empty() && current_section != "question" && !line.empty() && line[0] != ';') {
            // Record format: name TTL class type value
            std::istringstream rss(line);
            std::string name, ttl_str, rclass, type;
            if (rss >> name >> ttl_str >> rclass >> type) {
                DnsQueryRecord rec;
                rec.name = name;
                try { rec.ttl = std::stoi(ttl_str); } catch (...) {}
                rec.type = type;
                rec.section = current_section;
                // Rest of line is the value
                std::string val;
                std::getline(rss, val);
                if (!val.empty() && val[0] == ' ') val = val.substr(1);
                // Handle IN class appearing as part of the parse
                if (rclass != "IN" && rclass != "CH" && rclass != "HS") {
                    // ttl_str might actually be class, shift fields
                    rec.type = rclass;
                    val = type + " " + val;
                }
                rec.value = val;
                result.records.push_back(rec);
            }
        }

        // Empty line resets section
        if (line.empty()) current_section.clear();
    }

    return result;
}

Result<DnsQueryResult, std::string> DnsManager::lookup(const std::string& domain,
                                                        const std::string& type,
                                                        const std::string& server) const {
    std::string cmd = "dig";
    if (!server.empty()) cmd += " @" + server;
    cmd += " " + domain;
    if (!type.empty()) cmd += " " + type;
    cmd += " +noall +comments +answer +authority +additional +stats 2>&1";

    std::string output = run_cmd(cmd);
    if (output.empty())
        return Result<DnsQueryResult, std::string>::error("dig command failed or not found");

    DnsQueryResult result = parse_dig_output(output);
    result.query_name = domain;
    result.query_type = type.empty() ? "A" : type;
    if (result.status.empty()) result.status = "UNKNOWN";

    return Result<DnsQueryResult, std::string>::ok(result);
}

Result<DnsQueryResult, std::string> DnsManager::reverse(const std::string& ip) const {
    std::string cmd = "dig -x " + ip + " +noall +comments +answer +stats 2>&1";
    std::string output = run_cmd(cmd);
    if (output.empty())
        return Result<DnsQueryResult, std::string>::error("reverse lookup failed");

    DnsQueryResult result = parse_dig_output(output);
    result.query_name = ip;
    result.query_type = "PTR";
    return Result<DnsQueryResult, std::string>::ok(result);
}

Result<std::vector<DnsQueryResult>, std::string> DnsManager::trace(
    const std::string& domain) const {
    std::string cmd = "dig " + domain + " +trace +nodnssec 2>&1";
    std::string output = run_cmd(cmd);
    if (output.empty())
        return Result<std::vector<DnsQueryResult>, std::string>::error("trace failed");

    // Split output by delegation steps (separated by blank lines with new sections)
    std::vector<DnsQueryResult> steps;
    std::istringstream stream(output);
    std::string line, block;

    while (std::getline(stream, line)) {
        if (line.empty() && !block.empty()) {
            DnsQueryResult step = parse_dig_output(block);
            if (!step.records.empty()) {
                step.query_name = domain;
                steps.push_back(step);
            }
            block.clear();
        } else {
            block += line + "\n";
        }
    }
    if (!block.empty()) {
        DnsQueryResult step = parse_dig_output(block);
        if (!step.records.empty()) {
            step.query_name = domain;
            steps.push_back(step);
        }
    }

    return Result<std::vector<DnsQueryResult>, std::string>::ok(steps);
}

Result<DnsServerConfig, std::string> DnsManager::config() const {
    DnsServerConfig cfg;

    // Check systemd-resolved first
    if (fs::exists("/run/systemd/resolve/stub-resolv.conf")) {
        cfg.mode = "systemd-resolved";
        std::string output = run_cmd("resolvectl status 2>/dev/null");
        if (!output.empty()) {
            std::istringstream stream(output);
            std::string line;
            while (std::getline(stream, line)) {
                if (line.find("DNS Servers:") != std::string::npos ||
                    line.find("Current DNS Server:") != std::string::npos) {
                    std::regex ip_re(R"((\d+\.\d+\.\d+\.\d+))");
                    auto it = std::sregex_iterator(line.begin(), line.end(), ip_re);
                    for (; it != std::sregex_iterator(); ++it)
                        cfg.servers.push_back((*it)[1].str());
                }
                if (line.find("DNS Domain:") != std::string::npos) {
                    auto colon = line.find(':');
                    if (colon != std::string::npos) {
                        std::istringstream ds(line.substr(colon + 1));
                        std::string d;
                        while (ds >> d) cfg.search_domains.push_back(d);
                    }
                }
                if (line.find("DNSSEC") != std::string::npos && line.find("yes") != std::string::npos)
                    cfg.dnssec = true;
                if (line.find("over TLS") != std::string::npos && line.find("yes") != std::string::npos)
                    cfg.dns_over_tls = true;
            }
        }
    } else {
        cfg.mode = "resolvconf";
    }

    // Also parse /etc/resolv.conf
    std::ifstream f("/etc/resolv.conf");
    if (f.is_open()) {
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream ss(line);
            std::string key, val;
            ss >> key >> val;
            if (key == "nameserver" && !val.empty()) {
                bool found = false;
                for (const auto& s : cfg.servers) { if (s == val) { found = true; break; } }
                if (!found) cfg.servers.push_back(val);
            }
            if (key == "search") {
                while (ss >> val) cfg.search_domains.push_back(val);
            }
        }
    }

    return Result<DnsServerConfig, std::string>::ok(cfg);
}

Result<void, std::string> DnsManager::set_servers(const std::vector<std::string>& servers) const {
    if (servers.empty())
        return Result<void, std::string>::error("no servers specified");

    // Try systemd-resolved first
    if (fs::exists("/run/systemd/resolve/stub-resolv.conf")) {
        std::string cmd = "resolvectl dns eth0";
        for (const auto& s : servers) cmd += " " + s;
        cmd += " 2>&1";
        std::string output = run_cmd(cmd);
        // Try alternative interface names
        if (output.find("No such") != std::string::npos) {
            cmd = "resolvectl dns enp0s31f6";
            for (const auto& s : servers) cmd += " " + s;
            run_cmd(cmd + " 2>&1");
        }
        return Result<void, std::string>::ok();
    }

    // Fall back to writing /etc/resolv.conf
    std::ofstream f("/etc/resolv.conf");
    if (!f.is_open())
        return Result<void, std::string>::error("cannot write /etc/resolv.conf (permission denied?)");
    f << "# Generated by straylight-dns\n";
    for (const auto& s : servers) f << "nameserver " << s << "\n";
    if (f.fail())
        return Result<void, std::string>::error("write failed to /etc/resolv.conf");
    return Result<void, std::string>::ok();
}

Result<void, std::string> DnsManager::flush_cache() const {
    // systemd-resolved
    std::string output = run_cmd("resolvectl flush-caches 2>&1");
    if (output.find("not found") == std::string::npos &&
        output.find("error") == std::string::npos) {
        return Result<void, std::string>::ok();
    }
    // nscd
    output = run_cmd("nscd -i hosts 2>&1");
    if (output.find("not found") == std::string::npos) {
        return Result<void, std::string>::ok();
    }
    return Result<void, std::string>::error("no supported DNS cache found to flush");
}

Result<std::vector<CacheEntry>, std::string> DnsManager::cache_dump() const {
    std::vector<CacheEntry> entries;
    std::string output = run_cmd("resolvectl statistics 2>/dev/null");
    if (output.empty())
        return Result<std::vector<CacheEntry>, std::string>::error(
            "DNS cache inspection not available (systemd-resolved not running)");

    // resolvectl doesn't expose individual cache entries easily,
    // but we can show cache statistics
    output = run_cmd("resolvectl query --cache 2>/dev/null");
    // Parse what we can; this is best-effort
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty() || line[0] == '-') continue;
        std::istringstream ss(line);
        std::string name, type, value;
        if (ss >> name >> type >> value) {
            CacheEntry ce;
            ce.name = name;
            ce.type = type;
            ce.value = value;
            entries.push_back(ce);
        }
    }
    return Result<std::vector<CacheEntry>, std::string>::ok(entries);
}

Result<std::vector<PropagationResult>, std::string> DnsManager::propagation(
    const std::string& domain, const std::string& type,
    const std::string& expected) const {

    auto servers = public_dns_servers();
    std::vector<PropagationResult> results;

    // Server name mapping
    std::vector<std::string> names = {
        "Google", "Cloudflare", "Quad9", "OpenDNS",
        "Google-2", "Cloudflare-2"
    };

    for (size_t i = 0; i < servers.size(); ++i) {
        PropagationResult pr;
        pr.server = servers[i];
        pr.location = (i < names.size()) ? names[i] : "Unknown";

        auto start = std::chrono::steady_clock::now();
        std::string cmd = "dig @" + servers[i] + " " + domain + " " + type
                        + " +short +time=3 +tries=1 2>/dev/null";
        std::string output = run_cmd(cmd);
        auto end = std::chrono::steady_clock::now();

        pr.response_time_ms = std::chrono::duration<double, std::milli>(end - start).count();

        // Trim whitespace
        while (!output.empty() && (output.back() == '\n' || output.back() == ' '))
            output.pop_back();
        pr.value = output;
        pr.matches = (!expected.empty() && output.find(expected) != std::string::npos);

        results.push_back(pr);
    }

    return Result<std::vector<PropagationResult>, std::string>::ok(results);
}

Result<std::string, std::string> DnsManager::benchmark(const std::string& domain) const {
    std::ostringstream report;
    report << "DNS Benchmark for: " << domain << "\n\n";

    auto servers = public_dns_servers();
    std::vector<std::string> names = {
        "Google (8.8.8.8)", "Cloudflare (1.1.1.1)", "Quad9 (9.9.9.9)",
        "OpenDNS (208.67.222.222)", "Google-2 (8.8.4.4)", "Cloudflare-2 (1.0.0.1)"
    };

    // Also include system resolver
    servers.insert(servers.begin(), "");
    names.insert(names.begin(), "System Resolver");

    struct BenchResult {
        std::string name;
        double avg_ms;
        double min_ms;
        double max_ms;
        int failures;
    };

    std::vector<BenchResult> results;
    int iterations = 3;

    for (size_t i = 0; i < servers.size(); ++i) {
        BenchResult br;
        br.name = names[i];
        br.avg_ms = 0;
        br.min_ms = 1e9;
        br.max_ms = 0;
        br.failures = 0;

        for (int j = 0; j < iterations; ++j) {
            std::string cmd = "dig";
            if (!servers[i].empty()) cmd += " @" + servers[i];
            cmd += " " + domain + " A +noall +stats +time=3 +tries=1 2>&1";

            auto start = std::chrono::steady_clock::now();
            std::string output = run_cmd(cmd);
            auto end = std::chrono::steady_clock::now();
            double ms = std::chrono::duration<double, std::milli>(end - start).count();

            // Try to parse actual query time from dig
            std::regex time_re(R"(Query time:\s+(\d+)\s+msec)");
            std::smatch m;
            if (std::regex_search(output, m, time_re))
                ms = std::stod(m[1].str());

            if (output.find("timed out") != std::string::npos ||
                output.find("connection refused") != std::string::npos) {
                br.failures++;
            } else {
                br.avg_ms += ms;
                if (ms < br.min_ms) br.min_ms = ms;
                if (ms > br.max_ms) br.max_ms = ms;
            }
        }

        int successes = iterations - br.failures;
        if (successes > 0) br.avg_ms /= successes;
        else { br.min_ms = 0; br.max_ms = 0; }
        results.push_back(br);
    }

    // Sort by average time
    std::sort(results.begin(), results.end(),
              [](const auto& a, const auto& b) { return a.avg_ms < b.avg_ms; });

    report << std::left;
    char line[128];
    snprintf(line, sizeof(line), "%-28s %8s %8s %8s %8s\n",
             "SERVER", "AVG", "MIN", "MAX", "FAIL");
    report << line;
    report << std::string(60, '-') << "\n";

    for (const auto& r : results) {
        snprintf(line, sizeof(line), "%-28s %6.1fms %6.1fms %6.1fms %5d/%d\n",
                 r.name.c_str(), r.avg_ms, r.min_ms, r.max_ms,
                 r.failures, iterations);
        report << line;
    }

    if (!results.empty() && results[0].failures < iterations) {
        report << "\nFastest: " << results[0].name
               << " (" << results[0].avg_ms << "ms avg)\n";
    }

    return Result<std::string, std::string>::ok(report.str());
}

} // namespace straylight
