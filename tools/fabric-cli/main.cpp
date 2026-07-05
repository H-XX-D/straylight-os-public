/**
 * StrayLight Fabric CLI — Command-line interface for the device topology daemon.
 *
 * Commands:
 *   topology                    Show device graph as ASCII tree
 *   path <from> <to>           Find optimal route with per-hop details
 *   bottleneck <from> <to>     Find the slowest link between two devices
 *   affinity <device>          Show which CPU/NUMA node is closest
 *   devices [type]             List all devices, optionally filtered by type
 *   bandwidth <from> <to>      Show effective bandwidth between two devices
 */

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

const char* DAEMON_CMD_PATH  = "/var/run/straylight/fabric.sock.cmd";
const char* DAEMON_RESP_PATH = "/var/run/straylight/fabric.sock.resp";
const char* TOPOLOGY_JSON    = "/var/lib/straylight/fabric/topology.json";

// ── IPC with daemon ─────────────────────────────────────────────────

bool send_command(const std::string& cmd) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(fs::path(DAEMON_CMD_PATH).parent_path(), ec);

    std::ofstream out(DAEMON_CMD_PATH, std::ios::app);
    if (!out) {
        fprintf(stderr, "error: cannot write to %s — is straylight-fabric running?\n",
                DAEMON_CMD_PATH);
        return false;
    }
    out << cmd << "\n";
    out.close();
    return true;
}

std::string read_response(int timeout_ms = 3000) {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto start = std::chrono::steady_clock::now();

    while (true) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed > timeout_ms) break;

        if (fs::exists(DAEMON_RESP_PATH, ec) && fs::file_size(DAEMON_RESP_PATH, ec) > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            std::ifstream in(DAEMON_RESP_PATH);
            std::string content((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
            in.close();
            std::ofstream clear(DAEMON_RESP_PATH, std::ios::trunc);
            return content;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return "";
}

std::string send_and_receive(const std::string& cmd) {
    { std::ofstream clear(DAEMON_RESP_PATH, std::ios::trunc); }
    if (!send_command(cmd)) return "";
    return read_response();
}

// ── Formatting helpers ──────────────────────────────────────────────

std::string format_bw(double gbps) {
    if (gbps >= 1000.0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.1f Tbps", gbps / 1000.0);
        return buf;
    }
    if (gbps >= 1.0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.1f Gbps", gbps);
        return buf;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f Mbps", gbps * 1000.0);
    return buf;
}

std::string format_latency(double ns) {
    if (ns >= 1000000.0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.2f ms", ns / 1000000.0);
        return buf;
    }
    if (ns >= 1000.0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.2f us", ns / 1000.0);
        return buf;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%.0f ns", ns);
    return buf;
}

// ── ASCII topology tree ─────────────────────────────────────────────

struct SimpleNode {
    std::string id;
    std::string type;
    std::string name;
    double bandwidth_gbps = 0;
    int numa_node = -1;
};

struct SimpleEdge {
    std::string from;
    std::string to;
    double bandwidth_gbps = 0;
    double latency_ns = 0;
    std::string type;
};

void print_ascii_topology(const std::vector<SimpleNode>& nodes,
                           const std::vector<SimpleEdge>& edges) {
    // Group nodes by type
    std::map<std::string, std::vector<const SimpleNode*>> by_type;
    for (auto& n : nodes) {
        by_type[n.type].push_back(&n);
    }

    // Build adjacency for tree rendering
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> children;
    // pcie:root is the central hub
    for (auto& e : edges) {
        children[e.from].push_back({e.to, e.type + " " + format_bw(e.bandwidth_gbps)});
    }

    // Print header
    printf("\n");
    printf("  StrayLight Fabric — Device Topology\n");
    printf("  ====================================\n\n");

    // Print as grouped tree
    std::string root = "pcie:root";
    bool has_root = false;
    for (auto& n : nodes) {
        if (n.id == root) { has_root = true; break; }
    }

    if (has_root) {
        // Tree from root
        printf("  [PCIe Root Complex]\n");
        auto& kids = children[root];

        // Group children by type for cleaner display
        std::map<std::string, std::vector<std::pair<std::string, std::string>>> type_groups;
        for (auto& [child_id, link_info] : kids) {
            for (auto& n : nodes) {
                if (n.id == child_id) {
                    type_groups[n.type].push_back({child_id, link_info});
                    break;
                }
            }
        }

        int group_idx = 0;
        int total_groups = static_cast<int>(type_groups.size());
        for (auto& [type, group_kids] : type_groups) {
            bool last_group = (++group_idx == total_groups);
            std::string prefix = last_group ? "  +-- " : "  |-- ";
            std::string indent = last_group ? "      " : "  |   ";

            // Print type header
            std::string type_upper = type;
            if (!type_upper.empty()) type_upper[0] = static_cast<char>(toupper(type_upper[0]));
            printf("%s[%s]\n", prefix.c_str(), type_upper.c_str());

            for (size_t i = 0; i < group_kids.size(); ++i) {
                auto& [child_id, link_info] = group_kids[i];
                bool last_child = (i + 1 == group_kids.size());
                std::string child_prefix = last_child ? (indent + "+-- ") : (indent + "|-- ");

                // Find name
                std::string name = child_id;
                double bw = 0;
                int numa = -1;
                for (auto& n : nodes) {
                    if (n.id == child_id) {
                        name = n.name.empty() ? child_id : n.name;
                        bw = n.bandwidth_gbps;
                        numa = n.numa_node;
                        break;
                    }
                }

                printf("%s%s", child_prefix.c_str(), name.c_str());
                printf("  (%s", link_info.c_str());
                if (numa >= 0) printf(", NUMA %d", numa);
                printf(")\n");
            }
        }
    }

    // Also print CPUs and memory if not shown
    printf("\n");
    for (auto& [type, group] : by_type) {
        if (type == "pcie_root") continue;
        bool shown = false;
        if (has_root) {
            // Check if already shown as child of root
            for (auto& n : group) {
                auto& kids = children[root];
                for (auto& [kid, _] : kids) {
                    if (kid == n->id) { shown = true; break; }
                }
                if (shown) break;
            }
        }
        if (shown) continue;

        // Print standalone groups
        for (auto& n : group) {
            if (type == "cpu" || type == "numa_node" || type == "memory") {
                printf("  [%s] %s", n->id.c_str(), n->name.c_str());
                if (n->bandwidth_gbps > 0)
                    printf("  (%s)", format_bw(n->bandwidth_gbps).c_str());
                if (n->numa_node >= 0)
                    printf("  NUMA %d", n->numa_node);
                printf("\n");
            }
        }
    }

    printf("\n  Total: %zu devices, %zu links\n\n",
           nodes.size(), edges.size());
}

// ── Parse daemon response into simple structures ────────────────────

std::vector<SimpleNode> parse_device_list(const std::string& resp) {
    std::vector<SimpleNode> nodes;
    std::istringstream iss(resp);
    std::string line;
    std::getline(iss, line); // skip "DEVICES: N" header

    while (std::getline(iss, line)) {
        if (line.empty() || line[0] != ' ') continue;
        // Format: "  id [type] name BW Gbps numa=N"
        std::istringstream lss(line);
        SimpleNode node;
        lss >> node.id;

        // Read [type]
        std::string type_bracket;
        lss >> type_bracket;
        if (type_bracket.size() > 2 && type_bracket.front() == '[') {
            node.type = type_bracket.substr(1, type_bracket.size() - 2);
        }

        // Rest is name and properties
        std::string rest;
        std::getline(lss, rest);
        // Trim leading space
        auto f = rest.find_first_not_of(' ');
        if (f != std::string::npos) rest = rest.substr(f);
        node.name = rest;

        // Extract bandwidth if present
        auto gbps_pos = rest.find(" Gbps");
        if (gbps_pos != std::string::npos) {
            // Find the number before "Gbps"
            auto start = rest.rfind(' ', gbps_pos - 1);
            if (start == std::string::npos) start = 0;
            try {
                node.bandwidth_gbps = std::stod(rest.substr(start));
            } catch (...) {}
        }

        // Extract NUMA
        auto numa_pos = rest.find("numa=");
        if (numa_pos != std::string::npos) {
            try {
                node.numa_node = std::stoi(rest.substr(numa_pos + 5));
            } catch (...) {}
        }

        nodes.push_back(std::move(node));
    }
    return nodes;
}

// ── Commands ────────────────────────────────────────────────────────

int cmd_topology(int /*argc*/, char* /*argv*/[]) {
    // Get all devices from daemon
    auto resp = send_and_receive("devices");
    if (resp.empty()) {
        // Try reading cached topology JSON
        std::ifstream f(TOPOLOGY_JSON);
        if (!f) {
            fprintf(stderr, "error: cannot reach daemon and no cached topology\n");
            return 1;
        }
        printf("(using cached topology from %s)\n", TOPOLOGY_JSON);
        std::string json((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
        printf("%s", json.c_str());
        return 0;
    }

    auto nodes = parse_device_list(resp);

    // Get edges by requesting the full JSON topology
    auto json_resp = send_and_receive("topology");
    std::vector<SimpleEdge> edges;

    // Parse edges from JSON (simple extraction)
    auto parse_edges_from_json = [&](const std::string& json) {
        size_t pos = json.find("\"edges\"");
        if (pos == std::string::npos) return;
        pos = json.find('[', pos);
        if (pos == std::string::npos) return;

        while ((pos = json.find('{', pos)) != std::string::npos) {
            auto end = json.find('}', pos);
            if (end == std::string::npos) break;
            auto block = json.substr(pos, end - pos + 1);

            SimpleEdge edge;
            // Extract from
            auto extract = [&](const std::string& key) -> std::string {
                auto kp = block.find("\"" + key + "\"");
                if (kp == std::string::npos) return {};
                auto colon = block.find(':', kp);
                if (colon == std::string::npos) return {};
                auto q1 = block.find('"', colon);
                if (q1 == std::string::npos) return {};
                auto q2 = block.find('"', q1 + 1);
                if (q2 == std::string::npos) return {};
                return block.substr(q1 + 1, q2 - q1 - 1);
            };
            auto extract_num = [&](const std::string& key) -> double {
                auto kp = block.find("\"" + key + "\"");
                if (kp == std::string::npos) return 0;
                auto colon = block.find(':', kp);
                if (colon == std::string::npos) return 0;
                size_t start = colon + 1;
                while (start < block.size() && block[start] == ' ') ++start;
                try { return std::stod(block.substr(start)); }
                catch (...) { return 0; }
            };

            edge.from = extract("from");
            edge.to = extract("to");
            edge.bandwidth_gbps = extract_num("bandwidth_gbps");
            edge.latency_ns = extract_num("latency_ns");
            edge.type = extract("type");

            if (!edge.from.empty() && !edge.to.empty()) {
                edges.push_back(std::move(edge));
            }
            pos = end + 1;
        }
    };

    if (!json_resp.empty()) {
        parse_edges_from_json(json_resp);
    }

    print_ascii_topology(nodes, edges);
    return 0;
}

int cmd_path(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "usage: fabric-cli path <from> <to>\n");
        return 1;
    }
    std::string from = argv[0];
    std::string to = argv[1];

    auto resp = send_and_receive("path " + from + " " + to);
    if (resp.empty()) {
        fprintf(stderr, "error: no response from daemon\n");
        return 1;
    }

    // Format the response nicely
    std::istringstream iss(resp);
    std::string line;
    std::getline(iss, line);

    if (line.find("ERROR") != std::string::npos) {
        fprintf(stderr, "%s\n", line.c_str());
        return 1;
    }

    printf("\n  Optimal Path: %s -> %s\n", from.c_str(), to.c_str());
    printf("  %s\n\n", line.c_str());

    int hop = 0;
    while (std::getline(iss, line)) {
        if (line.empty()) continue;
        printf("  hop %d: %s\n", hop++, line.c_str());
    }
    printf("\n");
    return 0;
}

int cmd_bottleneck(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "usage: fabric-cli bottleneck <from> <to>\n");
        return 1;
    }

    auto resp = send_and_receive("bottleneck " + std::string(argv[0]) + " " + argv[1]);
    if (resp.empty()) {
        fprintf(stderr, "error: no response from daemon\n");
        return 1;
    }
    printf("\n  %s\n", resp.c_str());
    return resp.find("ERROR") != std::string::npos ? 1 : 0;
}

int cmd_affinity(int argc, char* argv[]) {
    if (argc < 1) {
        fprintf(stderr, "usage: fabric-cli affinity <device>\n");
        return 1;
    }

    auto resp = send_and_receive("affinity " + std::string(argv[0]));
    if (resp.empty()) {
        fprintf(stderr, "error: no response from daemon\n");
        return 1;
    }

    if (resp.find("AFFINITY:") != std::string::npos) {
        // Parse and format
        printf("\n  Device Affinity\n");
        printf("  ===============\n");
        // Extract fields
        auto extract = [&](const std::string& key) -> std::string {
            auto pos = resp.find(key + "=");
            if (pos == std::string::npos) return "N/A";
            auto start = pos + key.size() + 1;
            auto end = resp.find(' ', start);
            if (end == std::string::npos) end = resp.find('\n', start);
            if (end == std::string::npos) end = resp.size();
            return resp.substr(start, end - start);
        };
        printf("  Device:      %s\n", extract("device").c_str());
        printf("  Closest CPU: %s\n", extract("cpu").c_str());
        printf("  NUMA Node:   %s\n", extract("numa").c_str());
        printf("  Latency:     %s\n\n", extract("latency").c_str());
    } else {
        printf("%s\n", resp.c_str());
    }
    return resp.find("ERROR") != std::string::npos ? 1 : 0;
}

int cmd_devices(int argc, char* argv[]) {
    std::string type;
    if (argc >= 1) type = argv[0];

    std::string cmd = "devices";
    if (!type.empty()) cmd += " " + type;

    auto resp = send_and_receive(cmd);
    if (resp.empty()) {
        fprintf(stderr, "error: no response from daemon\n");
        return 1;
    }

    // Format as table
    auto nodes = parse_device_list(resp);

    printf("\n  %-28s  %-12s  %-30s  %12s  %s\n",
           "ID", "TYPE", "NAME", "BANDWIDTH", "NUMA");
    printf("  %-28s  %-12s  %-30s  %12s  %s\n",
           "----------------------------", "------------",
           "------------------------------", "------------", "----");

    for (auto& n : nodes) {
        std::string bw_str = n.bandwidth_gbps > 0 ? format_bw(n.bandwidth_gbps) : "-";
        std::string numa_str = n.numa_node >= 0 ? std::to_string(n.numa_node) : "-";

        // Truncate name for display
        std::string display_name = n.name;
        if (display_name.size() > 30) display_name = display_name.substr(0, 27) + "...";

        printf("  %-28s  %-12s  %-30s  %12s  %s\n",
               n.id.c_str(), n.type.c_str(), display_name.c_str(),
               bw_str.c_str(), numa_str.c_str());
    }
    printf("\n  Total: %zu devices\n\n", nodes.size());
    return 0;
}

int cmd_bandwidth(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "usage: fabric-cli bandwidth <from> <to>\n");
        return 1;
    }

    auto resp = send_and_receive("bandwidth " + std::string(argv[0]) + " " + argv[1]);
    if (resp.empty()) {
        fprintf(stderr, "error: no response from daemon\n");
        return 1;
    }

    if (resp.find("BANDWIDTH:") != std::string::npos) {
        auto pos = resp.find(':');
        auto val_str = resp.substr(pos + 2);
        double gbps = 0;
        try { gbps = std::stod(val_str); }
        catch (...) {}
        printf("\n  Effective bandwidth: %s -> %s = %s\n\n",
               argv[0], argv[1], format_bw(gbps).c_str());
    } else {
        printf("%s\n", resp.c_str());
    }
    return resp.find("ERROR") != std::string::npos ? 1 : 0;
}

void print_usage() {
    printf(
        "StrayLight Fabric CLI — Unified device topology\n"
        "\n"
        "Usage:\n"
        "  fabric-cli topology                    Show device graph as ASCII tree\n"
        "  fabric-cli path <from> <to>           Find optimal route with per-hop details\n"
        "  fabric-cli bottleneck <from> <to>     Find the slowest link between devices\n"
        "  fabric-cli affinity <device>          Show CPU/NUMA affinity for a device\n"
        "  fabric-cli devices [type]             List devices (types: gpu, nvme, nic, usb, cpu)\n"
        "  fabric-cli bandwidth <from> <to>      Show effective bandwidth between devices\n"
        "\n"
        "Device references can be:\n"
        "  - Full ID:     pci:0000:01:00.0, gpu:0, nvme:nvme0, nic:eth0\n"
        "  - Type name:   gpu, nvme, nic, cpu, memory\n"
        "  - Alias:       camera, encoder, disk, storage, network\n"
        "\n"
        "Examples:\n"
        "  fabric-cli topology\n"
        "  fabric-cli path gpu:0 nvme:nvme0\n"
        "  fabric-cli bottleneck nic:eth0 gpu:0\n"
        "  fabric-cli affinity gpu:0\n"
        "  fabric-cli devices gpu\n"
        "  fabric-cli bandwidth cpu:0 memory:system\n"
    );
}

} // anonymous namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string cmd = argv[1];
    int sub_argc = argc - 2;
    char** sub_argv = argv + 2;

    if (cmd == "topology")         return cmd_topology(sub_argc, sub_argv);
    if (cmd == "path")             return cmd_path(sub_argc, sub_argv);
    if (cmd == "bottleneck")       return cmd_bottleneck(sub_argc, sub_argv);
    if (cmd == "affinity")         return cmd_affinity(sub_argc, sub_argv);
    if (cmd == "devices")          return cmd_devices(sub_argc, sub_argv);
    if (cmd == "bandwidth")        return cmd_bandwidth(sub_argc, sub_argv);
    if (cmd == "--help" || cmd == "-h") {
        print_usage();
        return 0;
    }

    fprintf(stderr, "error: unknown command '%s'\n\n", cmd.c_str());
    print_usage();
    return 1;
}
