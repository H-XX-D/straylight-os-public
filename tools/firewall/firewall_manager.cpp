// tools/firewall/firewall_manager.cpp
// Full firewall manager implementation for StrayLight OS.

#include "firewall_manager.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>

namespace straylight {

FirewallManager::FirewallManager() = default;
FirewallManager::~FirewallManager() = default;

std::string FirewallManager::run_cmd(const std::string& cmd) const {
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return result;
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) result += buf;
    pclose(pipe);
    return result;
}

std::string FirewallManager::detect_backend() const {
    // Prefer nftables
    std::string nft_test = run_cmd("nft list tables 2>/dev/null");
    if (!nft_test.empty() || run_cmd("which nft 2>/dev/null").find("nft") != std::string::npos)
        return "nftables";
    // Fall back to iptables
    std::string ipt_test = run_cmd("iptables -V 2>/dev/null");
    if (!ipt_test.empty()) return "iptables";
    return "none";
}

std::string FirewallManager::chain_name(const std::string& chain) const {
    if (chain == "input" || chain == "INPUT") return "INPUT";
    if (chain == "output" || chain == "OUTPUT") return "OUTPUT";
    if (chain == "forward" || chain == "FORWARD") return "FORWARD";
    return chain;
}

Result<FwStatus, std::string> FirewallManager::status() const {
    FwStatus st;
    st.backend = detect_backend();

    if (st.backend == "nftables") {
        std::string output = run_cmd("nft list ruleset 2>/dev/null");
        st.active = !output.empty() && output.find("table") != std::string::npos;

        // Parse default policies from chain definitions
        std::regex chain_re(R"(chain\s+(\w+)\s*\{[^}]*type\s+filter[^}]*policy\s+(\w+))");
        auto it = std::sregex_iterator(output.begin(), output.end(), chain_re);
        for (; it != std::sregex_iterator(); ++it) {
            std::string name = (*it)[1].str();
            std::string policy = (*it)[2].str();
            std::transform(name.begin(), name.end(), name.begin(), ::tolower);
            if (name == "input") st.default_input = policy;
            else if (name == "output") st.default_output = policy;
            else if (name == "forward") st.default_forward = policy;
        }

        auto rules = parse_nft_list(output);
        st.total_rules = static_cast<int>(rules.size());

    } else if (st.backend == "iptables") {
        std::string output = run_cmd("iptables -L -n --line-numbers 2>/dev/null");
        st.active = !output.empty();

        // Parse default policies
        std::regex policy_re(R"(Chain\s+(\w+)\s+\(policy\s+(\w+)\))");
        auto it = std::sregex_iterator(output.begin(), output.end(), policy_re);
        for (; it != std::sregex_iterator(); ++it) {
            std::string name = (*it)[1].str();
            std::string policy = (*it)[2].str();
            std::transform(policy.begin(), policy.end(), policy.begin(), ::tolower);
            if (name == "INPUT") st.default_input = policy;
            else if (name == "OUTPUT") st.default_output = policy;
            else if (name == "FORWARD") st.default_forward = policy;
        }

        auto rules = parse_ipt_list(output);
        st.total_rules = static_cast<int>(rules.size());
    }

    return Result<FwStatus, std::string>::ok(st);
}

std::vector<FwRule> FirewallManager::parse_nft_list(const std::string& output) const {
    std::vector<FwRule> rules;
    std::istringstream stream(output);
    std::string line;
    std::string current_chain;
    uint32_t rule_id = 1;

    while (std::getline(stream, line)) {
        // Track which chain we're in
        std::regex chain_re(R"(chain\s+(\w+))");
        std::smatch cm;
        if (std::regex_search(line, cm, chain_re)) {
            current_chain = cm[1].str();
            std::transform(current_chain.begin(), current_chain.end(),
                          current_chain.begin(), ::tolower);
        }

        // Match rule lines (simplified)
        if (line.find("accept") != std::string::npos ||
            line.find("drop") != std::string::npos ||
            line.find("reject") != std::string::npos) {

            // Skip chain/table definitions
            if (line.find("policy") != std::string::npos) continue;
            if (line.find("chain ") != std::string::npos) continue;

            FwRule rule;
            rule.id = rule_id++;
            rule.chain = current_chain;

            if (line.find("accept") != std::string::npos) rule.action = "accept";
            else if (line.find("drop") != std::string::npos) rule.action = "drop";
            else if (line.find("reject") != std::string::npos) rule.action = "reject";

            // Parse protocol
            std::regex proto_re(R"(\b(tcp|udp|icmp)\b)");
            std::smatch pm;
            if (std::regex_search(line, pm, proto_re)) rule.protocol = pm[1].str();

            // Parse port
            std::regex port_re(R"(dport\s+(\d+))");
            std::smatch portm;
            if (std::regex_search(line, portm, port_re))
                rule.port = std::stoi(portm[1].str());

            // Parse source/dest
            std::regex src_re(R"(saddr\s+(\S+))");
            std::smatch sm;
            if (std::regex_search(line, sm, src_re)) rule.source = sm[1].str();

            std::regex dst_re(R"(daddr\s+(\S+))");
            if (std::regex_search(line, sm, dst_re)) rule.destination = sm[1].str();

            // Parse comment
            std::regex comment_re(R"(comment\s+"([^"]*)")");
            if (std::regex_search(line, sm, comment_re)) rule.comment = sm[1].str();

            // Parse counters
            std::regex counter_re(R"(packets\s+(\d+)\s+bytes\s+(\d+))");
            if (std::regex_search(line, sm, counter_re)) {
                rule.packets = std::stoull(sm[1].str());
                rule.bytes = std::stoull(sm[2].str());
            }

            rules.push_back(rule);
        }
    }
    return rules;
}

std::vector<FwRule> FirewallManager::parse_ipt_list(const std::string& output) const {
    std::vector<FwRule> rules;
    std::istringstream stream(output);
    std::string line;
    std::string current_chain;
    uint32_t rule_id = 1;

    while (std::getline(stream, line)) {
        // Detect chain
        if (line.rfind("Chain ", 0) == 0) {
            std::istringstream ss(line);
            std::string label, name;
            ss >> label >> name;
            current_chain = name;
            std::transform(current_chain.begin(), current_chain.end(),
                          current_chain.begin(), ::tolower);
            continue;
        }

        // Skip headers
        if (line.find("target") != std::string::npos && line.find("prot") != std::string::npos)
            continue;
        if (line.empty()) continue;

        // Parse iptables rule line: num target prot opt source destination [extras]
        std::istringstream ss(line);
        std::string num_str, target, prot, opt, src, dst;
        if (!(ss >> num_str >> target >> prot >> opt >> src >> dst)) continue;

        // Verify num_str is a number
        bool is_num = true;
        for (char c : num_str) { if (!std::isdigit(c)) { is_num = false; break; } }
        if (!is_num) continue;

        FwRule rule;
        rule.id = rule_id++;
        rule.chain = current_chain;

        std::transform(target.begin(), target.end(), target.begin(), ::tolower);
        rule.action = target;
        if (prot != "all") rule.protocol = prot;
        if (src != "0.0.0.0/0") rule.source = src;
        if (dst != "0.0.0.0/0") rule.destination = dst;

        // Parse remaining extras for port
        std::string rest;
        std::getline(ss, rest);
        std::regex dport_re(R"(dpt:(\d+))");
        std::smatch dm;
        if (std::regex_search(rest, dm, dport_re))
            rule.port = std::stoi(dm[1].str());

        // Parse comment
        std::regex comment_re(R"(/\*\s*(.*?)\s*\*/)");
        if (std::regex_search(rest, dm, comment_re))
            rule.comment = dm[1].str();

        rules.push_back(rule);
    }
    return rules;
}

Result<std::vector<FwRule>, std::string> FirewallManager::list_rules() const {
    std::string backend = detect_backend();
    if (backend == "nftables") {
        std::string output = run_cmd("nft list ruleset 2>/dev/null");
        return Result<std::vector<FwRule>, std::string>::ok(parse_nft_list(output));
    }
    if (backend == "iptables") {
        std::string output = run_cmd("iptables -L -n --line-numbers -v 2>/dev/null");
        return Result<std::vector<FwRule>, std::string>::ok(parse_ipt_list(output));
    }
    return Result<std::vector<FwRule>, std::string>::error("no firewall backend found");
}

Result<void, std::string> FirewallManager::nft_add_rule(const FwRule& rule) const {
    // Ensure table and chain exist
    run_cmd("nft add table inet straylight 2>/dev/null");
    std::string chain_lower = rule.chain.empty() ? "input" : rule.chain;
    std::string chain_upper = chain_name(chain_lower);
    std::transform(chain_lower.begin(), chain_lower.end(), chain_lower.begin(), ::tolower);

    run_cmd("nft 'add chain inet straylight " + chain_lower +
            " { type filter hook " + chain_lower + " priority 0; }' 2>/dev/null");

    std::string cmd = "nft add rule inet straylight " + chain_lower;
    if (!rule.protocol.empty()) cmd += " " + rule.protocol;
    if (!rule.source.empty()) cmd += " ip saddr " + rule.source;
    if (!rule.destination.empty()) cmd += " ip daddr " + rule.destination;
    if (rule.port > 0) cmd += " dport " + std::to_string(rule.port);
    if (!rule.interface.empty()) cmd += " iifname \"" + rule.interface + "\"";
    if (!rule.comment.empty()) cmd += " comment \"" + rule.comment + "\"";
    cmd += " counter " + rule.action;
    cmd += " 2>&1";

    std::string output = run_cmd(cmd);
    if (output.find("Error") != std::string::npos || output.find("error") != std::string::npos)
        return Result<void, std::string>::error("nftables: " + output);
    return Result<void, std::string>::ok();
}

Result<void, std::string> FirewallManager::ipt_add_rule(const FwRule& rule) const {
    std::string chain_upper = chain_name(rule.chain.empty() ? "input" : rule.chain);
    std::string action = rule.action;
    std::transform(action.begin(), action.end(), action.begin(), ::toupper);

    std::string cmd = "iptables -A " + chain_upper;
    if (!rule.protocol.empty()) cmd += " -p " + rule.protocol;
    if (!rule.source.empty()) cmd += " -s " + rule.source;
    if (!rule.destination.empty()) cmd += " -d " + rule.destination;
    if (rule.port > 0) {
        if (rule.protocol.empty()) cmd += " -p tcp";
        cmd += " --dport " + std::to_string(rule.port);
    }
    if (!rule.interface.empty()) cmd += " -i " + rule.interface;
    if (!rule.comment.empty()) cmd += " -m comment --comment \"" + rule.comment + "\"";
    cmd += " -j " + action;
    cmd += " 2>&1";

    std::string output = run_cmd(cmd);
    if (output.find("denied") != std::string::npos || output.find("Error") != std::string::npos)
        return Result<void, std::string>::error("iptables: " + output);
    return Result<void, std::string>::ok();
}

Result<void, std::string> FirewallManager::add_rule(const FwRule& rule) {
    std::string backend = detect_backend();
    if (backend == "nftables") return nft_add_rule(rule);
    if (backend == "iptables") return ipt_add_rule(rule);
    return Result<void, std::string>::error("no firewall backend found");
}

Result<void, std::string> FirewallManager::remove_rule(uint32_t id) {
    std::string backend = detect_backend();
    if (backend == "nftables") {
        // nftables rule handles need to be queried first
        std::string output = run_cmd("nft -a list ruleset 2>/dev/null");
        std::regex handle_re(R"(# handle (\d+))");
        auto it = std::sregex_iterator(output.begin(), output.end(), handle_re);
        uint32_t count = 0;
        for (; it != std::sregex_iterator(); ++it) {
            if (++count == id) {
                std::string handle = (*it)[1].str();
                // Need to find the table/chain for this handle
                // Simplified: try removing from straylight table
                std::string cmd = "nft delete rule inet straylight input handle " + handle + " 2>&1";
                run_cmd(cmd);
                return Result<void, std::string>::ok();
            }
        }
        return Result<void, std::string>::error("rule ID " + std::to_string(id) + " not found");
    }
    if (backend == "iptables") {
        // Map our sequential ID back to the chain + line number
        auto rules_res = list_rules();
        if (!rules_res.has_value()) return Result<void, std::string>::error(rules_res.error());

        for (const auto& r : rules_res.value()) {
            if (r.id == id) {
                std::string chain_upper = chain_name(r.chain);
                // Delete by specification (safe approach)
                std::string cmd = "iptables -D " + chain_upper;
                if (!r.protocol.empty()) cmd += " -p " + r.protocol;
                if (!r.source.empty()) cmd += " -s " + r.source;
                if (r.port > 0) cmd += " --dport " + std::to_string(r.port);
                std::string action = r.action;
                std::transform(action.begin(), action.end(), action.begin(), ::toupper);
                cmd += " -j " + action + " 2>&1";
                run_cmd(cmd);
                return Result<void, std::string>::ok();
            }
        }
        return Result<void, std::string>::error("rule ID " + std::to_string(id) + " not found");
    }
    return Result<void, std::string>::error("no firewall backend found");
}

Result<void, std::string> FirewallManager::set_default(const std::string& chain,
                                                        const std::string& policy) {
    std::string backend = detect_backend();
    std::string chain_upper = chain_name(chain);
    std::string policy_lower = policy;
    std::transform(policy_lower.begin(), policy_lower.end(), policy_lower.begin(), ::tolower);

    if (backend == "nftables") {
        std::string chain_lower = chain;
        std::transform(chain_lower.begin(), chain_lower.end(), chain_lower.begin(), ::tolower);
        run_cmd("nft add table inet straylight 2>/dev/null");
        std::string cmd = "nft 'add chain inet straylight " + chain_lower +
                          " { type filter hook " + chain_lower + " priority 0; policy " +
                          policy_lower + "; }' 2>&1";
        run_cmd(cmd);
        return Result<void, std::string>::ok();
    }
    if (backend == "iptables") {
        std::string policy_upper = policy;
        std::transform(policy_upper.begin(), policy_upper.end(), policy_upper.begin(), ::toupper);
        std::string cmd = "iptables -P " + chain_upper + " " + policy_upper + " 2>&1";
        std::string output = run_cmd(cmd);
        if (output.find("denied") != std::string::npos)
            return Result<void, std::string>::error("permission denied");
        return Result<void, std::string>::ok();
    }
    return Result<void, std::string>::error("no firewall backend found");
}

Result<void, std::string> FirewallManager::enable() {
    std::string backend = detect_backend();
    if (backend == "nftables") {
        run_cmd("nft add table inet straylight 2>/dev/null");
        run_cmd("nft 'add chain inet straylight input { type filter hook input priority 0; policy accept; }' 2>/dev/null");
        run_cmd("nft 'add chain inet straylight output { type filter hook output priority 0; policy accept; }' 2>/dev/null");
        run_cmd("nft 'add chain inet straylight forward { type filter hook forward priority 0; policy drop; }' 2>/dev/null");
        // Allow established connections
        run_cmd("nft add rule inet straylight input ct state established,related accept 2>/dev/null");
        // Allow loopback
        run_cmd("nft add rule inet straylight input iifname lo accept 2>/dev/null");
        return Result<void, std::string>::ok();
    }
    if (backend == "iptables") {
        run_cmd("iptables -A INPUT -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT 2>/dev/null");
        run_cmd("iptables -A INPUT -i lo -j ACCEPT 2>/dev/null");
        return Result<void, std::string>::ok();
    }
    return Result<void, std::string>::error("no firewall backend found");
}

Result<void, std::string> FirewallManager::disable() {
    std::string backend = detect_backend();
    if (backend == "nftables") {
        run_cmd("nft flush ruleset 2>/dev/null");
        return Result<void, std::string>::ok();
    }
    if (backend == "iptables") {
        run_cmd("iptables -F 2>/dev/null");
        run_cmd("iptables -P INPUT ACCEPT 2>/dev/null");
        run_cmd("iptables -P OUTPUT ACCEPT 2>/dev/null");
        run_cmd("iptables -P FORWARD ACCEPT 2>/dev/null");
        return Result<void, std::string>::ok();
    }
    return Result<void, std::string>::error("no firewall backend found");
}

Result<void, std::string> FirewallManager::reset() {
    auto disable_res = disable();
    if (!disable_res.has_value()) return disable_res;
    return enable();
}

Result<void, std::string> FirewallManager::add_forward(const FwForward& fwd) {
    std::string backend = detect_backend();
    if (backend == "nftables") {
        run_cmd("nft add table ip nat 2>/dev/null");
        run_cmd("nft 'add chain ip nat prerouting { type nat hook prerouting priority -100; }' 2>/dev/null");
        std::string proto = fwd.protocol.empty() ? "tcp" : fwd.protocol;
        std::string cmd = "nft add rule ip nat prerouting " + proto +
                          " dport " + std::to_string(fwd.external_port) +
                          " dnat to " + fwd.internal_ip + ":" +
                          std::to_string(fwd.internal_port) + " 2>&1";
        std::string output = run_cmd(cmd);
        if (output.find("Error") != std::string::npos)
            return Result<void, std::string>::error("nftables forward: " + output);
        return Result<void, std::string>::ok();
    }
    if (backend == "iptables") {
        std::string proto = fwd.protocol.empty() ? "tcp" : fwd.protocol;
        std::string cmd = "iptables -t nat -A PREROUTING -p " + proto +
                          " --dport " + std::to_string(fwd.external_port) +
                          " -j DNAT --to-destination " + fwd.internal_ip + ":" +
                          std::to_string(fwd.internal_port) + " 2>&1";
        run_cmd(cmd);
        return Result<void, std::string>::ok();
    }
    return Result<void, std::string>::error("no firewall backend found");
}

Result<std::vector<FwForward>, std::string> FirewallManager::list_forwards() const {
    std::vector<FwForward> forwards;
    std::string backend = detect_backend();

    if (backend == "nftables") {
        std::string output = run_cmd("nft list table ip nat 2>/dev/null");
        std::regex dnat_re(R"((\w+)\s+dport\s+(\d+)\s+dnat\s+to\s+(\S+):(\d+))");
        auto it = std::sregex_iterator(output.begin(), output.end(), dnat_re);
        uint32_t id = 1;
        for (; it != std::sregex_iterator(); ++it) {
            FwForward f;
            f.id = id++;
            f.protocol = (*it)[1].str();
            f.external_port = std::stoi((*it)[2].str());
            f.internal_ip = (*it)[3].str();
            f.internal_port = std::stoi((*it)[4].str());
            forwards.push_back(f);
        }
    } else if (backend == "iptables") {
        std::string output = run_cmd("iptables -t nat -L PREROUTING -n --line-numbers 2>/dev/null");
        std::regex dnat_re(R"(DNAT\s+(\w+)\s+.*dpt:(\d+)\s+to:(\S+):(\d+))");
        auto it = std::sregex_iterator(output.begin(), output.end(), dnat_re);
        uint32_t id = 1;
        for (; it != std::sregex_iterator(); ++it) {
            FwForward f;
            f.id = id++;
            f.protocol = (*it)[1].str();
            f.external_port = std::stoi((*it)[2].str());
            f.internal_ip = (*it)[3].str();
            f.internal_port = std::stoi((*it)[4].str());
            forwards.push_back(f);
        }
    }

    return Result<std::vector<FwForward>, std::string>::ok(forwards);
}

Result<void, std::string> FirewallManager::remove_forward(uint32_t id) {
    auto fwds = list_forwards();
    if (!fwds.has_value()) return Result<void, std::string>::error(fwds.error());

    for (const auto& f : fwds.value()) {
        if (f.id == id) {
            std::string backend = detect_backend();
            std::string proto = f.protocol.empty() ? "tcp" : f.protocol;
            if (backend == "nftables") {
                // Remove by handle (simplified)
                std::string cmd = "nft delete rule ip nat prerouting handle " +
                                  std::to_string(id) + " 2>&1";
                run_cmd(cmd);
            } else {
                std::string cmd = "iptables -t nat -D PREROUTING -p " + proto +
                                  " --dport " + std::to_string(f.external_port) +
                                  " -j DNAT --to-destination " + f.internal_ip + ":" +
                                  std::to_string(f.internal_port) + " 2>&1";
                run_cmd(cmd);
            }
            return Result<void, std::string>::ok();
        }
    }
    return Result<void, std::string>::error("forward rule " + std::to_string(id) + " not found");
}

Result<std::vector<ConnTrack>, std::string> FirewallManager::conntrack() const {
    std::vector<ConnTrack> entries;
    std::string output = run_cmd("conntrack -L 2>/dev/null");
    if (output.empty()) {
        // Try /proc/net/nf_conntrack
        std::ifstream f("/proc/net/nf_conntrack");
        if (f.is_open()) {
            std::string content((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
            output = content;
        }
    }

    if (output.empty())
        return Result<std::vector<ConnTrack>, std::string>::error(
            "connection tracking not available (conntrack tool missing?)");

    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        ConnTrack ct;

        // Parse protocol
        std::regex proto_re(R"(\b(tcp|udp|icmp)\b)");
        std::smatch m;
        if (std::regex_search(line, m, proto_re)) ct.protocol = m[1].str();

        // Parse state
        std::regex state_re(R"(\b(ESTABLISHED|TIME_WAIT|SYN_SENT|SYN_RECV|FIN_WAIT|CLOSE_WAIT|CLOSE|LAST_ACK)\b)");
        if (std::regex_search(line, m, state_re)) ct.state = m[1].str();

        // Parse src/dst/sport/dport
        std::regex src_re(R"(src=(\S+))");
        if (std::regex_search(line, m, src_re)) ct.source = m[1].str();
        std::regex dst_re(R"(dst=(\S+))");
        if (std::regex_search(line, m, dst_re)) ct.destination = m[1].str();
        std::regex sport_re(R"(sport=(\d+))");
        if (std::regex_search(line, m, sport_re)) ct.src_port = std::stoi(m[1].str());
        std::regex dport_re(R"(dport=(\d+))");
        if (std::regex_search(line, m, dport_re)) ct.dst_port = std::stoi(m[1].str());

        // Parse packets/bytes
        std::regex packets_re(R"(packets=(\d+))");
        if (std::regex_search(line, m, packets_re)) ct.packets = std::stoull(m[1].str());
        std::regex bytes_re(R"(bytes=(\d+))");
        if (std::regex_search(line, m, bytes_re)) ct.bytes = std::stoull(m[1].str());

        if (!ct.source.empty()) entries.push_back(ct);
    }

    return Result<std::vector<ConnTrack>, std::string>::ok(entries);
}

Result<std::string, std::string> FirewallManager::export_rules() const {
    std::string backend = detect_backend();
    if (backend == "nftables") {
        std::string output = run_cmd("nft list ruleset 2>/dev/null");
        if (output.empty())
            return Result<std::string, std::string>::error("failed to export nftables ruleset");
        return Result<std::string, std::string>::ok(output);
    }
    if (backend == "iptables") {
        std::string output = run_cmd("iptables-save 2>/dev/null");
        if (output.empty())
            return Result<std::string, std::string>::error("failed to export iptables rules");
        return Result<std::string, std::string>::ok(output);
    }
    return Result<std::string, std::string>::error("no firewall backend found");
}

Result<void, std::string> FirewallManager::import_rules(const std::string& rules_file) const {
    std::string backend = detect_backend();
    if (backend == "nftables") {
        std::string cmd = "nft -f " + rules_file + " 2>&1";
        std::string output = run_cmd(cmd);
        if (output.find("Error") != std::string::npos)
            return Result<void, std::string>::error("import failed: " + output);
        return Result<void, std::string>::ok();
    }
    if (backend == "iptables") {
        std::string cmd = "iptables-restore < " + rules_file + " 2>&1";
        std::string output = run_cmd(cmd);
        if (!output.empty())
            return Result<void, std::string>::error("import failed: " + output);
        return Result<void, std::string>::ok();
    }
    return Result<void, std::string>::error("no firewall backend found");
}

} // namespace straylight
