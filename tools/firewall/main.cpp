// tools/firewall/main.cpp
// CLI front-end for straylight-firewall — firewall manager.

#include "firewall_manager.h"

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>

// ---------------------------------------------------------------------------
// Usage
// ---------------------------------------------------------------------------

static void print_usage() {
    std::cerr
        << "straylight-firewall — firewall manager (nftables/iptables)\n"
        << "\n"
        << "Usage:\n"
        << "  straylight-firewall status                             Firewall status\n"
        << "  straylight-firewall list                               List all rules\n"
        << "  straylight-firewall allow [options]                    Add allow rule\n"
        << "  straylight-firewall deny [options]                     Add deny rule\n"
        << "  straylight-firewall remove <id>                        Remove rule\n"
        << "  straylight-firewall default <chain> <accept|drop>      Set default policy\n"
        << "  straylight-firewall enable                             Enable firewall\n"
        << "  straylight-firewall disable                            Disable firewall\n"
        << "  straylight-firewall reset                              Reset to defaults\n"
        << "  straylight-firewall forward [options]                  Add port forward\n"
        << "  straylight-firewall forwards                           List port forwards\n"
        << "  straylight-firewall unforward <id>                     Remove port forward\n"
        << "  straylight-firewall conntrack                          Connection tracking\n"
        << "  straylight-firewall export                             Export ruleset\n"
        << "  straylight-firewall import <file>                      Import ruleset\n"
        << "\n"
        << "Rule options:\n"
        << "  --port=N          Port number\n"
        << "  --proto=tcp|udp   Protocol (default: tcp)\n"
        << "  --from=IP         Source address\n"
        << "  --to=IP           Destination address\n"
        << "  --iface=X         Interface\n"
        << "  --chain=input     Chain (input/output/forward)\n"
        << "  --comment=X       Comment\n"
        << "\n"
        << "Forward options:\n"
        << "  --port=N          External port\n"
        << "  --to-ip=X         Internal IP\n"
        << "  --to-port=N       Internal port\n"
        << "  --proto=tcp|udp   Protocol\n";
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

static std::string human_bytes(uint64_t bytes) {
    const char* units[] = {"B", "KiB", "MiB", "GiB"};
    int idx = 0;
    double val = static_cast<double>(bytes);
    while (val >= 1024.0 && idx < 3) { val /= 1024.0; ++idx; }
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f %s", val, units[idx]);
    return buf;
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

    straylight::FirewallManager mgr;

    // -----------------------------------------------------------------------
    // status
    // -----------------------------------------------------------------------
    if (command == "status") {
        auto res = mgr.status();
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        const auto& st = res.value();
        std::cout << "Firewall Status:\n"
                  << "  Backend:  " << st.backend << "\n"
                  << "  Active:   " << (st.active ? "yes" : "no") << "\n"
                  << "  Rules:    " << st.total_rules << "\n"
                  << "\n  Default Policies:\n"
                  << "    Input:   " << (st.default_input.empty() ? "n/a" : st.default_input) << "\n"
                  << "    Output:  " << (st.default_output.empty() ? "n/a" : st.default_output) << "\n"
                  << "    Forward: " << (st.default_forward.empty() ? "n/a" : st.default_forward) << "\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // list
    // -----------------------------------------------------------------------
    if (command == "list") {
        auto res = mgr.list_rules();
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        const auto& rules = res.value();
        if (rules.empty()) {
            std::cout << "No firewall rules configured.\n";
            return 0;
        }
        std::cout << pad("ID", 6) << pad("CHAIN", 10) << pad("ACTION", 10)
                  << pad("PROTO", 8) << pad("PORT", 8) << pad("SOURCE", 18)
                  << pad("DEST", 18) << pad("PKTS", 10) << pad("BYTES", 12)
                  << "COMMENT\n"
                  << std::string(110, '-') << "\n";
        for (const auto& r : rules) {
            std::cout << pad(std::to_string(r.id), 6)
                      << pad(r.chain, 10)
                      << pad(r.action, 10)
                      << pad(r.protocol.empty() ? "any" : r.protocol, 8)
                      << pad(r.port > 0 ? std::to_string(r.port) : "*", 8)
                      << pad(r.source.empty() ? "*" : r.source, 18)
                      << pad(r.destination.empty() ? "*" : r.destination, 18)
                      << pad(std::to_string(r.packets), 10)
                      << pad(human_bytes(r.bytes), 12)
                      << r.comment << "\n";
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // allow / deny
    // -----------------------------------------------------------------------
    if (command == "allow" || command == "deny") {
        straylight::FwRule rule;
        rule.action = (command == "allow") ? "accept" : "drop";
        rule.chain = get_arg(argc, argv, "--chain=", 2);
        if (rule.chain.empty()) rule.chain = "input";

        std::string port_str = get_arg(argc, argv, "--port=", 2);
        if (!port_str.empty()) rule.port = std::stoi(port_str);
        rule.protocol = get_arg(argc, argv, "--proto=", 2);
        rule.source = get_arg(argc, argv, "--from=", 2);
        rule.destination = get_arg(argc, argv, "--to=", 2);
        rule.interface = get_arg(argc, argv, "--iface=", 2);
        rule.comment = get_arg(argc, argv, "--comment=", 2);
        if (rule.protocol.empty() && rule.port > 0) rule.protocol = "tcp";

        auto res = mgr.add_rule(rule);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Rule added: " << command;
        if (rule.port > 0) std::cout << " port " << rule.port << "/" << rule.protocol;
        if (!rule.source.empty()) std::cout << " from " << rule.source;
        std::cout << "\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // remove <id>
    // -----------------------------------------------------------------------
    if (command == "remove") {
        if (argc < 3) {
            std::cerr << "Error: 'remove' requires a rule ID\n";
            return 1;
        }
        uint32_t id = static_cast<uint32_t>(std::stoul(argv[2]));
        auto res = mgr.remove_rule(id);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Rule " << id << " removed.\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // default <chain> <policy>
    // -----------------------------------------------------------------------
    if (command == "default") {
        if (argc < 4) {
            std::cerr << "Error: 'default' requires <chain> and <policy>\n";
            return 1;
        }
        auto res = mgr.set_default(argv[2], argv[3]);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Default policy for " << argv[2] << " set to " << argv[3] << ".\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // enable / disable / reset
    // -----------------------------------------------------------------------
    if (command == "enable") {
        auto res = mgr.enable();
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Firewall enabled.\n";
        return 0;
    }
    if (command == "disable") {
        auto res = mgr.disable();
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Firewall disabled.\n";
        return 0;
    }
    if (command == "reset") {
        auto res = mgr.reset();
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Firewall reset to defaults.\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // forward [options]
    // -----------------------------------------------------------------------
    if (command == "forward") {
        straylight::FwForward fwd;
        std::string port_str = get_arg(argc, argv, "--port=", 2);
        if (!port_str.empty()) fwd.external_port = std::stoi(port_str);
        fwd.internal_ip = get_arg(argc, argv, "--to-ip=", 2);
        std::string to_port = get_arg(argc, argv, "--to-port=", 2);
        if (!to_port.empty()) fwd.internal_port = std::stoi(to_port);
        fwd.protocol = get_arg(argc, argv, "--proto=", 2);
        fwd.interface = get_arg(argc, argv, "--iface=", 2);

        if (fwd.external_port == 0 || fwd.internal_ip.empty() || fwd.internal_port == 0) {
            std::cerr << "Error: 'forward' requires --port, --to-ip, and --to-port\n";
            return 1;
        }

        auto res = mgr.add_forward(fwd);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Port forward: " << fwd.external_port << " -> "
                  << fwd.internal_ip << ":" << fwd.internal_port << "\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // forwards
    // -----------------------------------------------------------------------
    if (command == "forwards") {
        auto res = mgr.list_forwards();
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        const auto& fwds = res.value();
        if (fwds.empty()) {
            std::cout << "No port forwards configured.\n";
            return 0;
        }
        std::cout << pad("ID", 6) << pad("PROTO", 8) << pad("EXT PORT", 10)
                  << pad("INTERNAL IP", 18) << "INT PORT\n"
                  << std::string(52, '-') << "\n";
        for (const auto& f : fwds) {
            std::cout << pad(std::to_string(f.id), 6)
                      << pad(f.protocol.empty() ? "tcp" : f.protocol, 8)
                      << pad(std::to_string(f.external_port), 10)
                      << pad(f.internal_ip, 18)
                      << f.internal_port << "\n";
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // unforward <id>
    // -----------------------------------------------------------------------
    if (command == "unforward") {
        if (argc < 3) {
            std::cerr << "Error: 'unforward' requires a forward rule ID\n";
            return 1;
        }
        uint32_t id = static_cast<uint32_t>(std::stoul(argv[2]));
        auto res = mgr.remove_forward(id);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Port forward " << id << " removed.\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // conntrack
    // -----------------------------------------------------------------------
    if (command == "conntrack") {
        auto res = mgr.conntrack();
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        const auto& entries = res.value();
        if (entries.empty()) {
            std::cout << "No tracked connections.\n";
            return 0;
        }
        std::cout << pad("PROTO", 8) << pad("STATE", 14) << pad("SOURCE", 22)
                  << pad("DESTINATION", 22) << pad("PACKETS", 10)
                  << "BYTES\n"
                  << std::string(86, '-') << "\n";
        for (const auto& ct : entries) {
            std::string src = ct.source;
            if (ct.src_port > 0) src += ":" + std::to_string(ct.src_port);
            std::string dst = ct.destination;
            if (ct.dst_port > 0) dst += ":" + std::to_string(ct.dst_port);

            std::cout << pad(ct.protocol, 8)
                      << pad(ct.state.empty() ? "-" : ct.state, 14)
                      << pad(src, 22)
                      << pad(dst, 22)
                      << pad(std::to_string(ct.packets), 10)
                      << human_bytes(ct.bytes) << "\n";
        }
        std::cout << "\n" << entries.size() << " tracked connections\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // export
    // -----------------------------------------------------------------------
    if (command == "export") {
        auto res = mgr.export_rules();
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << res.value();
        return 0;
    }

    // -----------------------------------------------------------------------
    // import <file>
    // -----------------------------------------------------------------------
    if (command == "import") {
        if (argc < 3) {
            std::cerr << "Error: 'import' requires a rules file\n";
            return 1;
        }
        auto res = mgr.import_rules(argv[2]);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Rules imported from " << argv[2] << ".\n";
        return 0;
    }

    std::cerr << "Error: unknown command '" << command << "'\n";
    print_usage();
    return 1;
}
