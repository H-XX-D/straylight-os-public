/**
 * StrayLight Fabric — Device Topology Graph
 *
 * Every hardware device is a node with bandwidth/latency edges.
 * Supports scanning the full system topology from sysfs, Dijkstra
 * shortest-path (latency-weighted), max-bandwidth path, DMA chain
 * computation, and JSON serialization.
 */
#pragma once

#include "straylight/result.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace straylight::fabric {

// ── Device types ────────────────────────────────────────────────────

enum class DeviceType {
    CPU,
    GPU,
    NVMe,
    NIC,
    USB,
    Memory,
    PCIeSwitch,
    PCIeRoot,
    NUMANode,
    CacheLevel,
    Unknown
};

inline std::string device_type_str(DeviceType t) {
    switch (t) {
        case DeviceType::CPU:        return "cpu";
        case DeviceType::GPU:        return "gpu";
        case DeviceType::NVMe:       return "nvme";
        case DeviceType::NIC:        return "nic";
        case DeviceType::USB:        return "usb";
        case DeviceType::Memory:     return "memory";
        case DeviceType::PCIeSwitch: return "pcie_switch";
        case DeviceType::PCIeRoot:   return "pcie_root";
        case DeviceType::NUMANode:   return "numa_node";
        case DeviceType::CacheLevel: return "cache";
        case DeviceType::Unknown:    return "unknown";
    }
    return "unknown";
}

inline DeviceType device_type_from_str(const std::string& s) {
    if (s == "cpu")         return DeviceType::CPU;
    if (s == "gpu")         return DeviceType::GPU;
    if (s == "nvme")        return DeviceType::NVMe;
    if (s == "nic")         return DeviceType::NIC;
    if (s == "usb")         return DeviceType::USB;
    if (s == "memory")      return DeviceType::Memory;
    if (s == "pcie_switch") return DeviceType::PCIeSwitch;
    if (s == "pcie_root")   return DeviceType::PCIeRoot;
    if (s == "numa_node")   return DeviceType::NUMANode;
    if (s == "cache")       return DeviceType::CacheLevel;
    return DeviceType::Unknown;
}

// ── Edge (link) types ───────────────────────────────────────────────

enum class LinkType {
    PCIe,
    NVLink,
    Network,
    USB,
    SATA,
    MemoryBus,
    CacheBus,
    InterSocket,
    Unknown
};

inline std::string link_type_str(LinkType t) {
    switch (t) {
        case LinkType::PCIe:        return "pcie";
        case LinkType::NVLink:      return "nvlink";
        case LinkType::Network:     return "network";
        case LinkType::USB:         return "usb";
        case LinkType::SATA:        return "sata";
        case LinkType::MemoryBus:   return "memory_bus";
        case LinkType::CacheBus:    return "cache_bus";
        case LinkType::InterSocket: return "inter_socket";
        case LinkType::Unknown:     return "unknown";
    }
    return "unknown";
}

// ── Data structures ─────────────────────────────────────────────────

struct DeviceNode {
    std::string id;                  // unique identifier (e.g., "pci:0000:01:00.0")
    DeviceType  type = DeviceType::Unknown;
    std::string name;                // human-readable name
    std::map<std::string, std::string> properties; // arbitrary key-value

    // Common properties extracted for fast access
    double bandwidth_gbps = 0.0;     // device's own bandwidth capability
    double latency_ns     = 0.0;     // device's own access latency
    uint64_t capacity_bytes = 0;     // storage/memory capacity
    int numa_node         = -1;      // NUMA affinity
};

struct TopologyEdge {
    std::string from;
    std::string to;
    double bandwidth_gbps = 0.0;
    double latency_ns     = 0.0;
    LinkType type         = LinkType::Unknown;
};

struct PathHop {
    std::string from;
    std::string to;
    double bandwidth_gbps = 0.0;
    double latency_ns     = 0.0;
    LinkType link_type    = LinkType::Unknown;
};

struct TopologyPath {
    std::vector<PathHop> hops;
    double total_latency_ns    = 0.0;
    double bottleneck_bw_gbps  = 0.0;   // minimum bandwidth along the path
};

struct LocalityIsland {
    std::string id;                      // e.g. "island:numa0"
    int numa_node = -1;
    std::vector<std::string> cpu_ids;
    std::vector<std::string> memory_ids;
    std::vector<std::string> pcie_root_ids;
    std::vector<std::string> device_ids;
    uint64_t memory_bytes = 0;
    double memory_bandwidth_gbps = 0.0;
    std::map<std::string, std::string> properties;
};

// ── Topology graph ──────────────────────────────────────────────────

class Topology {
public:
    Topology() = default;

    /** Build the full system topology by scanning sysfs. */
    VoidResult<> build_topology() {
        std::lock_guard lock(mu_);
        nodes_.clear();
        edges_.clear();
        adjacency_.clear();

        scan_pcie_devices();
        scan_gpus();
        scan_nvme_devices();
        scan_nics();
        scan_usb_devices();
        scan_memory();
        scan_cpus();
        scan_numa_topology();

        build_locality_islands();
        build_adjacency();
        return VoidResult<>::ok();
    }

    /** Add a device node. */
    void add_node(DeviceNode node) {
        std::lock_guard lock(mu_);
        auto id = node.id;
        nodes_[id] = std::move(node);
    }

    /** Add an edge between two devices. */
    void add_edge(TopologyEdge edge) {
        std::lock_guard lock(mu_);
        edges_.push_back(edge);
        adjacency_[edge.from].push_back(edges_.size() - 1);
        // Bidirectional
        TopologyEdge rev = edge;
        std::swap(rev.from, rev.to);
        edges_.push_back(rev);
        adjacency_[rev.from].push_back(edges_.size() - 1);
    }

    /** Remove a device and all its edges. */
    void remove_node(const std::string& id) {
        std::lock_guard lock(mu_);
        nodes_.erase(id);
        // Remove edges involving this node and rebuild adjacency
        edges_.erase(
            std::remove_if(edges_.begin(), edges_.end(),
                [&](const TopologyEdge& e) {
                    return e.from == id || e.to == id;
                }),
            edges_.end());
        build_adjacency();
    }

    /** Find shortest path by latency (Dijkstra). */
    Result<TopologyPath, std::string> find_path(const std::string& from,
                                                 const std::string& to) const {
        std::lock_guard lock(mu_);
        return dijkstra_latency(from, to);
    }

    /** Find path with maximum bandwidth. */
    Result<TopologyPath, std::string> find_fastest_path(const std::string& from,
                                                         const std::string& to) const {
        std::lock_guard lock(mu_);
        return max_bandwidth_path(from, to);
    }

    /** Get optimal DMA chain for zero-copy transfer. */
    Result<TopologyPath, std::string> get_dma_chain(const std::string& source,
                                                     const std::string& sink) const {
        // DMA chain prefers PCIe paths with maximum bandwidth
        std::lock_guard lock(mu_);
        return max_bandwidth_path(source, sink);
    }

    /** Get all nodes. */
    std::vector<DeviceNode> get_all_nodes() const {
        std::lock_guard lock(mu_);
        std::vector<DeviceNode> result;
        result.reserve(nodes_.size());
        for (auto& [_, n] : nodes_) result.push_back(n);
        return result;
    }

    /** Get all edges. */
    std::vector<TopologyEdge> get_all_edges() const {
        std::lock_guard lock(mu_);
        return edges_;
    }

    /** Get a specific node by ID. */
    Result<DeviceNode, std::string> get_node(const std::string& id) const {
        std::lock_guard lock(mu_);
        auto it = nodes_.find(id);
        if (it == nodes_.end())
            return Result<DeviceNode, std::string>::error("device not found: " + id);
        return Result<DeviceNode, std::string>::ok(it->second);
    }

    /** Get all nodes of a specific type. */
    std::vector<DeviceNode> get_by_type(DeviceType type) const {
        std::lock_guard lock(mu_);
        std::vector<DeviceNode> result;
        for (auto& [_, n] : nodes_) {
            if (n.type == type) result.push_back(n);
        }
        return result;
    }

    /** Get all GPUs. */
    std::vector<DeviceNode> get_all_gpus() const { return get_by_type(DeviceType::GPU); }

    /** Get all storage devices. */
    std::vector<DeviceNode> get_all_storage() const { return get_by_type(DeviceType::NVMe); }

    /** Get all NICs. */
    std::vector<DeviceNode> get_all_nics() const { return get_by_type(DeviceType::NIC); }

    /** Get all CPUs. */
    std::vector<DeviceNode> get_all_cpus() const { return get_by_type(DeviceType::CPU); }

    std::vector<LocalityIsland> get_locality_islands() const {
        std::lock_guard lock(mu_);
        return locality_islands_;
    }

    size_t locality_island_count() const {
        std::lock_guard lock(mu_);
        return locality_islands_.size();
    }

    std::string locality_islands_to_json() const {
        std::lock_guard lock(mu_);
        std::ostringstream ss;
        write_locality_islands_json(ss);
        return ss.str();
    }

    /** Find node by fuzzy name match. */
    Result<std::string, std::string> resolve_device(const std::string& query) const {
        std::lock_guard lock(mu_);
        std::string lower_query;
        for (char c : query) lower_query += static_cast<char>(std::tolower(c));

        // Exact ID match
        if (nodes_.count(query)) return Result<std::string, std::string>::ok(query);

        // Name contains query (case-insensitive)
        std::string best_id;
        size_t best_score = 0;
        for (auto& [id, node] : nodes_) {
            std::string lower_name;
            for (char c : node.name) lower_name += static_cast<char>(std::tolower(c));
            std::string lower_id;
            for (char c : id) lower_id += static_cast<char>(std::tolower(c));

            // Check for substring match
            if (lower_name.find(lower_query) != std::string::npos) {
                size_t score = lower_query.size() * 100 / lower_name.size();
                if (score > best_score) {
                    best_score = score;
                    best_id = id;
                }
            }
            if (lower_id.find(lower_query) != std::string::npos) {
                size_t score = lower_query.size() * 100 / lower_id.size();
                if (score > best_score) {
                    best_score = score;
                    best_id = id;
                }
            }

            // Check type match
            std::string type_str = device_type_str(node.type);
            if (lower_query == type_str && best_score == 0) {
                best_id = id;
                best_score = 1;
            }
        }

        if (best_id.empty())
            return Result<std::string, std::string>::error("cannot resolve: " + query);
        return Result<std::string, std::string>::ok(best_id);
    }

    /** Serialize topology to JSON. */
    std::string to_json() const {
        std::lock_guard lock(mu_);
        std::ostringstream ss;
        ss << "{\n  \"nodes\": [\n";

        size_t ni = 0;
        for (auto& [id, node] : nodes_) {
            ss << "    {\n"
               << "      \"id\": \"" << id << "\",\n"
               << "      \"type\": \"" << device_type_str(node.type) << "\",\n"
               << "      \"name\": \"" << json_escape(node.name) << "\",\n"
               << "      \"bandwidth_gbps\": " << node.bandwidth_gbps << ",\n"
               << "      \"latency_ns\": " << node.latency_ns << ",\n"
               << "      \"capacity_bytes\": " << node.capacity_bytes << ",\n"
               << "      \"numa_node\": " << node.numa_node << ",\n"
               << "      \"properties\": {";

            size_t pi = 0;
            for (auto& [k, v] : node.properties) {
                if (pi > 0) ss << ",";
                ss << "\n        \"" << k << "\": \"" << json_escape(v) << "\"";
                ++pi;
            }
            if (!node.properties.empty()) ss << "\n      ";
            ss << "}\n    }";
            if (++ni < nodes_.size()) ss << ",";
            ss << "\n";
        }

        ss << "  ],\n  \"edges\": [\n";
        // Deduplicate (we store bidirectional)
        std::set<std::string> seen;
        size_t ei = 0;
        for (auto& e : edges_) {
            auto key = (e.from < e.to) ? (e.from + "|" + e.to) : (e.to + "|" + e.from);
            if (seen.count(key)) continue;
            seen.insert(key);

            if (ei > 0) ss << ",\n";
            ss << "    {\n"
               << "      \"from\": \"" << e.from << "\",\n"
               << "      \"to\": \"" << e.to << "\",\n"
               << "      \"bandwidth_gbps\": " << e.bandwidth_gbps << ",\n"
               << "      \"latency_ns\": " << e.latency_ns << ",\n"
               << "      \"type\": \"" << link_type_str(e.type) << "\"\n"
               << "    }";
            ++ei;
        }
        ss << "\n  ],\n  \"locality_islands\": ";
        write_locality_islands_json(ss);
        ss << "\n}\n";
        return ss.str();
    }

    /** Deserialize topology from JSON (minimal parser). */
    VoidResult<> from_json(const std::string& json) {
        std::lock_guard lock(mu_);
        nodes_.clear();
        edges_.clear();
        adjacency_.clear();

        // Minimal parse: find "nodes" and "edges" arrays
        auto parse_nodes = [&](const std::string& s) {
            // Simple state machine for JSON array of objects
            size_t pos = s.find("\"nodes\"");
            if (pos == std::string::npos) return;
            pos = s.find('[', pos);
            if (pos == std::string::npos) return;

            while ((pos = s.find('{', pos)) != std::string::npos) {
                auto end = s.find('}', pos);
                // Find nested properties object
                auto props_start = s.find("\"properties\"", pos);
                if (props_start != std::string::npos && props_start < end) {
                    auto inner_brace = s.find('{', props_start);
                    if (inner_brace != std::string::npos) {
                        auto inner_end = s.find('}', inner_brace);
                        if (inner_end != std::string::npos && inner_end > end) {
                            end = s.find('}', inner_end + 1);
                        }
                    }
                }
                if (end == std::string::npos) break;

                auto block = s.substr(pos, end - pos + 1);
                DeviceNode node;
                node.id = extract_json_string(block, "id");
                node.type = device_type_from_str(extract_json_string(block, "type"));
                node.name = extract_json_string(block, "name");
                node.bandwidth_gbps = extract_json_double(block, "bandwidth_gbps");
                node.latency_ns = extract_json_double(block, "latency_ns");
                node.capacity_bytes = static_cast<uint64_t>(
                    extract_json_double(block, "capacity_bytes"));
                node.numa_node = static_cast<int>(extract_json_double(block, "numa_node"));
                nodes_[node.id] = std::move(node);

                pos = end + 1;
                // Stop at the end of the nodes array
                auto bracket = s.find(']', pos);
                auto next_brace = s.find('{', pos);
                if (bracket != std::string::npos &&
                    (next_brace == std::string::npos || bracket < next_brace)) {
                    break;
                }
            }
        };

        auto parse_edges = [&](const std::string& s) {
            size_t pos = s.find("\"edges\"");
            if (pos == std::string::npos) return;
            pos = s.find('[', pos);
            if (pos == std::string::npos) return;

            while ((pos = s.find('{', pos)) != std::string::npos) {
                auto end = s.find('}', pos);
                if (end == std::string::npos) break;

                auto block = s.substr(pos, end - pos + 1);
                TopologyEdge edge;
                edge.from = extract_json_string(block, "from");
                edge.to = extract_json_string(block, "to");
                edge.bandwidth_gbps = extract_json_double(block, "bandwidth_gbps");
                edge.latency_ns = extract_json_double(block, "latency_ns");
                auto type_str = extract_json_string(block, "type");
                if (type_str == "pcie") edge.type = LinkType::PCIe;
                else if (type_str == "nvlink") edge.type = LinkType::NVLink;
                else if (type_str == "network") edge.type = LinkType::Network;
                else if (type_str == "usb") edge.type = LinkType::USB;
                else if (type_str == "sata") edge.type = LinkType::SATA;
                else if (type_str == "memory_bus") edge.type = LinkType::MemoryBus;
                else if (type_str == "cache_bus") edge.type = LinkType::CacheBus;
                else if (type_str == "inter_socket") edge.type = LinkType::InterSocket;

                edges_.push_back(edge);
                pos = end + 1;
            }
        };

        parse_nodes(json);
        parse_edges(json);
        build_adjacency();
        return VoidResult<>::ok();
    }

    size_t node_count() const {
        std::lock_guard lock(mu_);
        return nodes_.size();
    }

    size_t edge_count() const {
        std::lock_guard lock(mu_);
        return edges_.size() / 2; // stored bidirectional
    }

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, DeviceNode> nodes_;
    std::vector<TopologyEdge> edges_;
    std::unordered_map<std::string, std::vector<size_t>> adjacency_; // node -> edge indices
    std::vector<LocalityIsland> locality_islands_;

    void build_adjacency() {
        adjacency_.clear();
        for (size_t i = 0; i < edges_.size(); ++i) {
            adjacency_[edges_[i].from].push_back(i);
        }
    }

    void build_locality_islands() {
        locality_islands_.clear();
        std::unordered_map<std::string, size_t> by_id;

        auto ensure_island = [&](int numa_node) -> LocalityIsland& {
            const std::string id = (numa_node >= 0)
                ? ("island:numa" + std::to_string(numa_node))
                : std::string("island:unknown");
            auto it = by_id.find(id);
            if (it != by_id.end()) return locality_islands_[it->second];

            LocalityIsland island;
            island.id = id;
            island.numa_node = numa_node;
            island.properties["owner"] = "fabric";
            island.properties["policy"] = "observe_only";
            island.properties["placement_contract"] = "straylight.fabric.placement.v1";
            if (numa_node >= 0) {
                island.properties["cpulist"] = read_sysfs_string(
                    std::filesystem::path("/sys/devices/system/node") /
                    ("node" + std::to_string(numa_node)) / "cpulist");
            }

            by_id[id] = locality_islands_.size();
            locality_islands_.push_back(std::move(island));
            return locality_islands_.back();
        };

        auto add_unique = [](std::vector<std::string>& values, const std::string& value) {
            if (value.empty()) return;
            if (std::find(values.begin(), values.end(), value) == values.end()) {
                values.push_back(value);
            }
        };

        for (auto& [id, node] : nodes_) {
            if (node.type != DeviceType::NUMANode) continue;
            auto& island = ensure_island(node.numa_node);
            island.memory_bytes = std::max(island.memory_bytes, node.capacity_bytes);
            add_unique(island.memory_ids, id);
        }

        for (auto& [id, node] : nodes_) {
            int numa_node = node.numa_node;
            if (numa_node < 0 && node.properties.count("package")) {
                try { numa_node = std::stoi(node.properties.at("package")); }
                catch (...) {}
            }

            auto& island = ensure_island(numa_node);
            node.properties["placement_plane"] = island.id;
            node.properties["placement_owner"] = "fabric";

            switch (node.type) {
                case DeviceType::CPU:
                    add_unique(island.cpu_ids, id);
                    break;
                case DeviceType::NUMANode:
                case DeviceType::Memory:
                    add_unique(island.memory_ids, id);
                    island.memory_bytes = std::max(island.memory_bytes, node.capacity_bytes);
                    if (node.bandwidth_gbps > 0) {
                        island.memory_bandwidth_gbps =
                            std::max(island.memory_bandwidth_gbps, node.bandwidth_gbps);
                    }
                    break;
                case DeviceType::PCIeRoot:
                    add_unique(island.pcie_root_ids, id);
                    break;
                default:
                    add_unique(island.device_ids, id);
                    break;
            }
        }

        std::sort(locality_islands_.begin(), locality_islands_.end(),
                  [](const LocalityIsland& a, const LocalityIsland& b) {
                      return a.id < b.id;
                  });
    }

    // ── Sysfs scanners ──────────────────────────────────────────────

    void scan_pcie_devices() {
        namespace fs = std::filesystem;
        std::error_code ec;
        auto pci_dir = fs::path("/sys/bus/pci/devices");
        if (!fs::exists(pci_dir, ec)) return;

        // Add PCIe root complex
        DeviceNode root;
        root.id = "pcie:root";
        root.type = DeviceType::PCIeRoot;
        root.name = "PCIe Root Complex";
        root.bandwidth_gbps = 64.0; // PCIe 4.0 x16
        nodes_[root.id] = root;

        for (auto& entry : fs::directory_iterator(pci_dir, ec)) {
            auto bdf = entry.path().filename().string(); // e.g., 0000:01:00.0
            auto device_path = entry.path();

            // Read device class
            auto class_val = read_sysfs_string(device_path / "class");
            auto vendor_val = read_sysfs_string(device_path / "vendor");
            auto device_val = read_sysfs_string(device_path / "device");

            // Read link speed and width
            auto link_speed = read_sysfs_string(device_path / "current_link_speed");
            auto link_width = read_sysfs_string(device_path / "current_link_width");

            // Classify device
            DeviceType type = classify_pci_device(class_val);
            if (type == DeviceType::Unknown) continue; // skip uninteresting devices

            DeviceNode node;
            node.id = "pci:" + bdf;
            node.type = type;
            node.name = get_pci_device_name(vendor_val, device_val, type);
            node.bandwidth_gbps = compute_pcie_bandwidth(link_speed, link_width);
            node.properties["bdf"] = bdf;
            node.properties["class"] = class_val;
            node.properties["vendor"] = vendor_val;
            node.properties["device"] = device_val;
            node.properties["link_speed"] = link_speed;
            node.properties["link_width"] = link_width;

            // Determine NUMA node
            auto numa = read_sysfs_string(device_path / "numa_node");
            if (!numa.empty()) {
                try { node.numa_node = std::stoi(numa); }
                catch (...) {}
            }

            nodes_[node.id] = node;

            // Connect to root complex
            TopologyEdge edge;
            edge.from = "pcie:root";
            edge.to = node.id;
            edge.bandwidth_gbps = node.bandwidth_gbps;
            edge.latency_ns = 100.0; // typical PCIe latency
            edge.type = LinkType::PCIe;
            edges_.push_back(edge);
            TopologyEdge rev = edge;
            std::swap(rev.from, rev.to);
            edges_.push_back(rev);
        }
    }

    void scan_gpus() {
        namespace fs = std::filesystem;
        std::error_code ec;

        // Check for NVIDIA GPUs via nvidia-smi
        FILE* pipe = popen("nvidia-smi --query-gpu=index,name,memory.total,pci.bus_id"
                           " --format=csv,noheader,nounits 2>/dev/null", "r");
        if (pipe) {
            char buffer[512];
            while (fgets(buffer, sizeof(buffer), pipe)) {
                std::istringstream iss(buffer);
                std::string idx_s, name, mem_s, pci_bus;
                std::getline(iss, idx_s, ',');
                std::getline(iss, name, ',');
                std::getline(iss, mem_s, ',');
                std::getline(iss, pci_bus);

                // Trim whitespace
                auto trim = [](std::string& s) {
                    auto f = s.find_first_not_of(" \t\r\n");
                    auto l = s.find_last_not_of(" \t\r\n");
                    if (f == std::string::npos) return;
                    s = s.substr(f, l - f + 1);
                };
                trim(idx_s); trim(name); trim(mem_s); trim(pci_bus);

                DeviceNode gpu;
                gpu.id = "gpu:" + idx_s;
                gpu.type = DeviceType::GPU;
                gpu.name = name;
                try { gpu.capacity_bytes = std::stoull(mem_s) * 1024ULL * 1024; }
                catch (...) {}
                gpu.bandwidth_gbps = 900.0; // HBM2e bandwidth estimate
                gpu.properties["pci_bus"] = pci_bus;
                gpu.properties["memory_mb"] = mem_s;

                nodes_[gpu.id] = gpu;

                // Connect to PCIe root
                TopologyEdge edge;
                edge.from = "pcie:root";
                edge.to = gpu.id;
                edge.bandwidth_gbps = 32.0; // PCIe 4.0 x16
                edge.latency_ns = 200.0;
                edge.type = LinkType::PCIe;
                edges_.push_back(edge);
                TopologyEdge rev = edge;
                std::swap(rev.from, rev.to);
                edges_.push_back(rev);
            }
            pclose(pipe);
        }

        // Also scan DRM devices
        auto drm_dir = fs::path("/sys/class/drm");
        if (fs::exists(drm_dir, ec)) {
            for (auto& entry : fs::directory_iterator(drm_dir, ec)) {
                auto card = entry.path().filename().string();
                if (card.find("card") != 0 || card.find('-') != std::string::npos)
                    continue;

                auto device_link = entry.path() / "device";
                if (!fs::exists(device_link, ec)) continue;

                auto real_device = fs::read_symlink(device_link, ec);
                if (ec) continue;

                // Check if we already have this as a PCI device
                auto bdf = real_device.filename().string();
                auto pci_id = "pci:" + bdf;
                if (nodes_.count(pci_id)) {
                    // Upgrade to GPU type
                    nodes_[pci_id].type = DeviceType::GPU;
                    if (nodes_[pci_id].name.find("GPU") == std::string::npos) {
                        nodes_[pci_id].name = "GPU (" + nodes_[pci_id].name + ")";
                    }
                }
            }
        }
    }

    void scan_nvme_devices() {
        namespace fs = std::filesystem;
        std::error_code ec;
        auto nvme_dir = fs::path("/sys/class/nvme");
        if (!fs::exists(nvme_dir, ec)) return;

        for (auto& entry : fs::directory_iterator(nvme_dir, ec)) {
            auto nvme_name = entry.path().filename().string();

            DeviceNode node;
            node.id = "nvme:" + nvme_name;
            node.type = DeviceType::NVMe;

            auto model = read_sysfs_string(entry.path() / "model");
            auto serial = read_sysfs_string(entry.path() / "serial");
            node.name = model.empty() ? nvme_name : model;
            node.properties["serial"] = serial;

            // Read transport and address
            auto transport = read_sysfs_string(entry.path() / "transport");
            auto address = read_sysfs_string(entry.path() / "address");
            node.properties["transport"] = transport;
            node.properties["address"] = address;

            // Estimate bandwidth: PCIe 4.0 x4 NVMe = ~8 GB/s
            node.bandwidth_gbps = 8.0 * 8; // 8 GB/s * 8 bits = 64 Gbps
            node.latency_ns = 10000.0; // ~10us for NVMe

            // Read NUMA node
            auto numa = read_sysfs_string(entry.path() / "numa_node");
            if (!numa.empty()) {
                try { node.numa_node = std::stoi(numa); }
                catch (...) {}
            }

            nodes_[node.id] = node;

            // Connect to PCIe root
            TopologyEdge edge;
            edge.from = "pcie:root";
            edge.to = node.id;
            edge.bandwidth_gbps = 64.0; // PCIe 4.0 x4
            edge.latency_ns = 500.0;
            edge.type = LinkType::PCIe;
            edges_.push_back(edge);
            TopologyEdge rev = edge;
            std::swap(rev.from, rev.to);
            edges_.push_back(rev);
        }
    }

    void scan_nics() {
        namespace fs = std::filesystem;
        std::error_code ec;
        auto net_dir = fs::path("/sys/class/net");
        if (!fs::exists(net_dir, ec)) return;

        for (auto& entry : fs::directory_iterator(net_dir, ec)) {
            auto iface = entry.path().filename().string();
            if (iface == "lo") continue; // skip loopback

            DeviceNode node;
            node.id = "nic:" + iface;
            node.type = DeviceType::NIC;
            node.name = iface;

            // Read link speed in Mbps
            auto speed_str = read_sysfs_string(entry.path() / "speed");
            double speed_mbps = 0;
            if (!speed_str.empty()) {
                try { speed_mbps = std::stod(speed_str); }
                catch (...) {}
            }
            node.bandwidth_gbps = speed_mbps / 1000.0;
            node.properties["speed_mbps"] = speed_str;

            // Read MAC address
            auto address = read_sysfs_string(entry.path() / "address");
            node.properties["mac"] = address;

            // Read MTU
            auto mtu = read_sysfs_string(entry.path() / "mtu");
            node.properties["mtu"] = mtu;

            // Read operational state
            auto operstate = read_sysfs_string(entry.path() / "operstate");
            node.properties["state"] = operstate;

            // Read NUMA node from device link
            auto device_link = entry.path() / "device";
            if (fs::exists(device_link, ec)) {
                auto numa = read_sysfs_string(
                    fs::path(device_link) / "numa_node");
                if (!numa.empty()) {
                    try { node.numa_node = std::stoi(numa); }
                    catch (...) {}
                }
            }

            nodes_[node.id] = node;

            // Connect to PCIe root (if PCI device)
            if (fs::exists(entry.path() / "device", ec)) {
                TopologyEdge edge;
                edge.from = "pcie:root";
                edge.to = node.id;
                edge.bandwidth_gbps = node.bandwidth_gbps;
                edge.latency_ns = 1000.0; // typical NIC latency
                edge.type = LinkType::PCIe;
                edges_.push_back(edge);
                TopologyEdge rev = edge;
                std::swap(rev.from, rev.to);
                edges_.push_back(rev);
            }
        }
    }

    void scan_usb_devices() {
        namespace fs = std::filesystem;
        std::error_code ec;
        auto usb_dir = fs::path("/sys/bus/usb/devices");
        if (!fs::exists(usb_dir, ec)) return;

        for (auto& entry : fs::directory_iterator(usb_dir, ec)) {
            auto dev_name = entry.path().filename().string();
            // Skip interfaces (they contain ':')
            if (dev_name.find(':') != std::string::npos) continue;

            auto product = read_sysfs_string(entry.path() / "product");
            auto manufacturer = read_sysfs_string(entry.path() / "manufacturer");
            auto speed = read_sysfs_string(entry.path() / "speed");

            DeviceNode node;
            node.id = "usb:" + dev_name;
            node.type = DeviceType::USB;
            node.name = product.empty() ? ("USB " + dev_name) : product;
            node.properties["manufacturer"] = manufacturer;
            node.properties["speed"] = speed;

            // USB speed in Mbps
            double speed_mbps = 0;
            if (!speed.empty()) {
                try { speed_mbps = std::stod(speed); }
                catch (...) {}
            }
            node.bandwidth_gbps = speed_mbps / 1000.0;

            nodes_[node.id] = node;

            // Connect to root
            TopologyEdge edge;
            edge.from = "pcie:root";
            edge.to = node.id;
            edge.bandwidth_gbps = node.bandwidth_gbps;
            edge.latency_ns = 1000.0;
            edge.type = LinkType::USB;
            edges_.push_back(edge);
            TopologyEdge rev = edge;
            std::swap(rev.from, rev.to);
            edges_.push_back(rev);
        }
    }

    void scan_memory() {
        namespace fs = std::filesystem;
        std::error_code ec;
        auto mem_dir = fs::path("/sys/devices/system/memory");
        if (!fs::exists(mem_dir, ec)) return;

        // Get total memory from /proc/meminfo instead of per-block
        uint64_t total_kb = 0;
        std::ifstream meminfo("/proc/meminfo");
        if (meminfo) {
            std::string line;
            while (std::getline(meminfo, line)) {
                if (line.substr(0, 9) == "MemTotal:") {
                    std::istringstream iss(line.substr(9));
                    iss >> total_kb;
                    break;
                }
            }
        }

        DeviceNode mem;
        mem.id = "memory:system";
        mem.type = DeviceType::Memory;
        mem.name = "System Memory";
        mem.capacity_bytes = total_kb * 1024;
        mem.bandwidth_gbps = 51.2; // DDR4-3200 dual channel estimate
        mem.latency_ns = 80.0;     // typical DRAM latency
        nodes_[mem.id] = mem;
    }

    void scan_cpus() {
        namespace fs = std::filesystem;
        std::error_code ec;
        auto cpu_dir = fs::path("/sys/devices/system/cpu");
        if (!fs::exists(cpu_dir, ec)) return;

        // Count online CPUs
        auto online_str = read_sysfs_string(cpu_dir / "online");
        int max_cpu = 0;

        for (auto& entry : fs::directory_iterator(cpu_dir, ec)) {
            auto name = entry.path().filename().string();
            if (name.size() < 4 || name.substr(0, 3) != "cpu") continue;
            try {
                int n = std::stoi(name.substr(3));
                max_cpu = std::max(max_cpu, n);
            } catch (...) { continue; }
        }

        // Create a single CPU node per physical package
        std::set<int> packages;
        for (int i = 0; i <= max_cpu; ++i) {
            auto pkg_str = read_sysfs_string(
                cpu_dir / ("cpu" + std::to_string(i)) / "topology" / "physical_package_id");
            int pkg = 0;
            try { pkg = std::stoi(pkg_str); }
            catch (...) {}
            packages.insert(pkg);
        }

        for (int pkg : packages) {
            DeviceNode cpu;
            cpu.id = "cpu:" + std::to_string(pkg);
            cpu.type = DeviceType::CPU;
            cpu.name = "CPU Package " + std::to_string(pkg);
            cpu.numa_node = pkg; // typically 1:1

            // Read CPU model from /proc/cpuinfo
            std::ifstream cpuinfo("/proc/cpuinfo");
            if (cpuinfo) {
                std::string line;
                while (std::getline(cpuinfo, line)) {
                    if (line.find("model name") != std::string::npos) {
                        auto colon = line.find(':');
                        if (colon != std::string::npos) {
                            auto val = line.substr(colon + 2);
                            cpu.name = val;
                        }
                        break;
                    }
                }
            }

            cpu.properties["package"] = std::to_string(pkg);
            cpu.properties["online_cpus"] = online_str;

            nodes_[cpu.id] = cpu;

            // CPU connects to memory
            if (nodes_.count("memory:system")) {
                TopologyEdge edge;
                edge.from = cpu.id;
                edge.to = "memory:system";
                edge.bandwidth_gbps = 51.2; // DDR4-3200
                edge.latency_ns = 80.0;
                edge.type = LinkType::MemoryBus;
                edges_.push_back(edge);
                TopologyEdge rev = edge;
                std::swap(rev.from, rev.to);
                edges_.push_back(rev);
            }

            // CPU connects to PCIe root
            if (nodes_.count("pcie:root")) {
                TopologyEdge edge;
                edge.from = cpu.id;
                edge.to = "pcie:root";
                edge.bandwidth_gbps = 64.0;
                edge.latency_ns = 50.0;
                edge.type = LinkType::PCIe;
                edges_.push_back(edge);
                TopologyEdge rev = edge;
                std::swap(rev.from, rev.to);
                edges_.push_back(rev);
            }
        }
    }

    void scan_numa_topology() {
        namespace fs = std::filesystem;
        std::error_code ec;
        auto node_dir = fs::path("/sys/devices/system/node");
        if (!fs::exists(node_dir, ec)) return;

        std::vector<int> numa_nodes;
        for (auto& entry : fs::directory_iterator(node_dir, ec)) {
            auto name = entry.path().filename().string();
            if (name.substr(0, 4) != "node") continue;
            int node_id = 0;
            try { node_id = std::stoi(name.substr(4)); }
            catch (...) { continue; }
            numa_nodes.push_back(node_id);

            DeviceNode numa;
            numa.id = "numa:" + std::to_string(node_id);
            numa.type = DeviceType::NUMANode;
            numa.name = "NUMA Node " + std::to_string(node_id);
            numa.numa_node = node_id;

            // Read meminfo for this node
            auto meminfo_path = entry.path() / "meminfo";
            std::ifstream meminfo(meminfo_path.string());
            if (meminfo) {
                std::string line;
                while (std::getline(meminfo, line)) {
                    if (line.find("MemTotal") != std::string::npos) {
                        std::istringstream iss(line);
                        std::string tok;
                        uint64_t val = 0;
                        // "Node X MemTotal: YYYY kB"
                        while (iss >> tok) {
                            try { val = std::stoull(tok); }
                            catch (...) {}
                        }
                        numa.capacity_bytes = val * 1024;
                        break;
                    }
                }
            }

            nodes_[numa.id] = numa;

            // Connect NUMA node to its CPU
            auto cpu_id = "cpu:" + std::to_string(node_id);
            if (nodes_.count(cpu_id)) {
                TopologyEdge edge;
                edge.from = numa.id;
                edge.to = cpu_id;
                edge.bandwidth_gbps = 51.2;
                edge.latency_ns = 10.0;
                edge.type = LinkType::MemoryBus;
                edges_.push_back(edge);
                TopologyEdge rev = edge;
                std::swap(rev.from, rev.to);
                edges_.push_back(rev);
            }
        }

        // Add inter-NUMA distances
        auto distance_path = node_dir / "node0" / "distance";
        if (fs::exists(distance_path, ec) && numa_nodes.size() > 1) {
            for (int i : numa_nodes) {
                auto dist_file = node_dir / ("node" + std::to_string(i)) / "distance";
                std::ifstream df(dist_file.string());
                if (!df) continue;
                int j = 0;
                int dist = 0;
                while (df >> dist) {
                    if (j < static_cast<int>(numa_nodes.size()) && j != i) {
                        auto from = "numa:" + std::to_string(i);
                        auto to = "numa:" + std::to_string(numa_nodes[j]);
                        // NUMA distance 10 = local, 20+ = remote
                        double latency = static_cast<double>(dist) * 10.0; // rough ns estimate
                        TopologyEdge edge;
                        edge.from = from;
                        edge.to = to;
                        edge.bandwidth_gbps = 25.0; // QPI/UPI bandwidth estimate
                        edge.latency_ns = latency;
                        edge.type = LinkType::InterSocket;
                        edges_.push_back(edge);
                    }
                    ++j;
                }
            }
        }
    }

    // ── Graph algorithms ────────────────────────────────────────────

    Result<TopologyPath, std::string> dijkstra_latency(
            const std::string& from, const std::string& to) const {
        if (!nodes_.count(from))
            return Result<TopologyPath, std::string>::error("source not found: " + from);
        if (!nodes_.count(to))
            return Result<TopologyPath, std::string>::error("sink not found: " + to);
        if (from == to) {
            TopologyPath p;
            p.total_latency_ns = 0;
            p.bottleneck_bw_gbps = std::numeric_limits<double>::max();
            return Result<TopologyPath, std::string>::ok(p);
        }

        using PQ = std::priority_queue<
            std::pair<double, std::string>,
            std::vector<std::pair<double, std::string>>,
            std::greater<>>;

        std::unordered_map<std::string, double> dist;
        std::unordered_map<std::string, std::string> prev_node;
        std::unordered_map<std::string, size_t> prev_edge;
        PQ pq;

        for (auto& [id, _] : nodes_) dist[id] = std::numeric_limits<double>::max();
        dist[from] = 0;
        pq.push({0, from});

        while (!pq.empty()) {
            auto [d, u] = pq.top();
            pq.pop();

            if (d > dist[u]) continue;
            if (u == to) break;

            auto adj_it = adjacency_.find(u);
            if (adj_it == adjacency_.end()) continue;

            for (size_t ei : adj_it->second) {
                auto& e = edges_[ei];
                double new_dist = dist[u] + e.latency_ns;
                if (new_dist < dist[e.to]) {
                    dist[e.to] = new_dist;
                    prev_node[e.to] = u;
                    prev_edge[e.to] = ei;
                    pq.push({new_dist, e.to});
                }
            }
        }

        if (dist[to] == std::numeric_limits<double>::max())
            return Result<TopologyPath, std::string>::error(
                "no path from " + from + " to " + to);

        // Reconstruct path
        TopologyPath path;
        path.total_latency_ns = dist[to];
        path.bottleneck_bw_gbps = std::numeric_limits<double>::max();

        std::vector<PathHop> hops;
        std::string current = to;
        while (current != from) {
            auto ei = prev_edge[current];
            auto& e = edges_[ei];
            PathHop hop;
            hop.from = e.from;
            hop.to = e.to;
            hop.bandwidth_gbps = e.bandwidth_gbps;
            hop.latency_ns = e.latency_ns;
            hop.link_type = e.type;
            hops.push_back(hop);
            path.bottleneck_bw_gbps = std::min(path.bottleneck_bw_gbps, e.bandwidth_gbps);
            current = prev_node[current];
        }

        std::reverse(hops.begin(), hops.end());
        path.hops = std::move(hops);
        return Result<TopologyPath, std::string>::ok(std::move(path));
    }

    Result<TopologyPath, std::string> max_bandwidth_path(
            const std::string& from, const std::string& to) const {
        if (!nodes_.count(from))
            return Result<TopologyPath, std::string>::error("source not found: " + from);
        if (!nodes_.count(to))
            return Result<TopologyPath, std::string>::error("sink not found: " + to);
        if (from == to) {
            TopologyPath p;
            p.total_latency_ns = 0;
            p.bottleneck_bw_gbps = std::numeric_limits<double>::max();
            return Result<TopologyPath, std::string>::ok(p);
        }

        // Modified Dijkstra: maximize minimum bandwidth along path
        // Use negative bandwidth so max-heap becomes useful with min-heap
        using PQ = std::priority_queue<
            std::pair<double, std::string>,
            std::vector<std::pair<double, std::string>>>;

        std::unordered_map<std::string, double> best_bw; // best bottleneck BW to reach node
        std::unordered_map<std::string, std::string> prev_node;
        std::unordered_map<std::string, size_t> prev_edge;
        PQ pq;

        for (auto& [id, _] : nodes_) best_bw[id] = 0;
        best_bw[from] = std::numeric_limits<double>::max();
        pq.push({std::numeric_limits<double>::max(), from});

        while (!pq.empty()) {
            auto [bw, u] = pq.top();
            pq.pop();

            if (bw < best_bw[u]) continue;
            if (u == to) break;

            auto adj_it = adjacency_.find(u);
            if (adj_it == adjacency_.end()) continue;

            for (size_t ei : adj_it->second) {
                auto& e = edges_[ei];
                double new_bw = std::min(best_bw[u], e.bandwidth_gbps);
                if (new_bw > best_bw[e.to]) {
                    best_bw[e.to] = new_bw;
                    prev_node[e.to] = u;
                    prev_edge[e.to] = ei;
                    pq.push({new_bw, e.to});
                }
            }
        }

        if (best_bw[to] == 0)
            return Result<TopologyPath, std::string>::error(
                "no path from " + from + " to " + to);

        TopologyPath path;
        path.bottleneck_bw_gbps = best_bw[to];
        path.total_latency_ns = 0;

        std::vector<PathHop> hops;
        std::string current = to;
        while (current != from) {
            auto ei = prev_edge[current];
            auto& e = edges_[ei];
            PathHop hop;
            hop.from = e.from;
            hop.to = e.to;
            hop.bandwidth_gbps = e.bandwidth_gbps;
            hop.latency_ns = e.latency_ns;
            hop.link_type = e.type;
            hops.push_back(hop);
            path.total_latency_ns += e.latency_ns;
            current = prev_node[current];
        }

        std::reverse(hops.begin(), hops.end());
        path.hops = std::move(hops);
        return Result<TopologyPath, std::string>::ok(std::move(path));
    }

    // ── Helpers ─────────────────────────────────────────────────────

    static std::string read_sysfs_string(const std::filesystem::path& path) {
        std::ifstream f(path);
        if (!f) return {};
        std::string val;
        std::getline(f, val);
        // Trim
        while (!val.empty() && (val.back() == '\n' || val.back() == '\r'
                                || val.back() == ' '))
            val.pop_back();
        return val;
    }

    static DeviceType classify_pci_device(const std::string& class_hex) {
        if (class_hex.empty()) return DeviceType::Unknown;
        // PCI class codes: 0x03xxxx = display, 0x02xxxx = network,
        // 0x01xxxx = storage, 0x0cxxxx = serial bus (USB), etc.
        uint32_t cls = 0;
        try { cls = std::stoul(class_hex, nullptr, 16); }
        catch (...) { return DeviceType::Unknown; }

        uint8_t base = (cls >> 16) & 0xFF;
        switch (base) {
            case 0x03: return DeviceType::GPU;
            case 0x02: return DeviceType::NIC;
            case 0x01: return DeviceType::NVMe; // mass storage
            case 0x0c: return DeviceType::USB;   // serial bus
            case 0x06: return DeviceType::PCIeSwitch; // bridge
            default:   return DeviceType::Unknown;
        }
    }

    static std::string get_pci_device_name(const std::string& vendor,
                                            const std::string& device,
                                            DeviceType type) {
        std::string prefix = device_type_str(type);
        prefix[0] = static_cast<char>(std::toupper(prefix[0]));
        return prefix + " [" + vendor + ":" + device + "]";
    }

    static double compute_pcie_bandwidth(const std::string& speed,
                                          const std::string& width) {
        // speed: "8.0 GT/s PCIe" or "16.0 GT/s PCIe"
        // width: "x16", "x8", etc.
        double gt_per_s = 0;
        try {
            gt_per_s = std::stod(speed);
        } catch (...) {}

        int lanes = 1;
        if (!width.empty() && width[0] == 'x') {
            try { lanes = std::stoi(width.substr(1)); }
            catch (...) {}
        }

        // PCIe encoding: Gen3/4 uses 128b/130b encoding
        // Bandwidth = GT/s * lanes * (128/130) / 8 (bytes) => GB/s
        // Convert to Gbps: * 8
        double bw_gbps = gt_per_s * lanes * (128.0 / 130.0);
        return bw_gbps;
    }

    static std::string json_escape(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:   out += c;
            }
        }
        return out;
    }

    static void write_string_array(std::ostringstream& ss,
                                   const std::vector<std::string>& values) {
        ss << "[";
        for (size_t i = 0; i < values.size(); ++i) {
            if (i > 0) ss << ", ";
            ss << "\"" << json_escape(values[i]) << "\"";
        }
        ss << "]";
    }

    void write_locality_islands_json(std::ostringstream& ss) const {
        ss << "[\n";
        for (size_t i = 0; i < locality_islands_.size(); ++i) {
            const auto& island = locality_islands_[i];
            ss << "    {\n"
               << "      \"id\": \"" << json_escape(island.id) << "\",\n"
               << "      \"numa_node\": " << island.numa_node << ",\n"
               << "      \"memory_bytes\": " << island.memory_bytes << ",\n"
               << "      \"memory_bandwidth_gbps\": " << island.memory_bandwidth_gbps << ",\n"
               << "      \"cpu_ids\": ";
            write_string_array(ss, island.cpu_ids);
            ss << ",\n      \"memory_ids\": ";
            write_string_array(ss, island.memory_ids);
            ss << ",\n      \"pcie_root_ids\": ";
            write_string_array(ss, island.pcie_root_ids);
            ss << ",\n      \"device_ids\": ";
            write_string_array(ss, island.device_ids);
            ss << ",\n      \"properties\": {";

            size_t pi = 0;
            for (auto& [k, v] : island.properties) {
                if (pi > 0) ss << ",";
                ss << "\n        \"" << json_escape(k) << "\": \""
                   << json_escape(v) << "\"";
                ++pi;
            }
            if (!island.properties.empty()) ss << "\n      ";
            ss << "}\n    }";
            if (i + 1 < locality_islands_.size()) ss << ",";
            ss << "\n";
        }
        ss << "  ]";
    }

    static std::string extract_json_string(const std::string& block,
                                            const std::string& key) {
        auto kpos = block.find("\"" + key + "\"");
        if (kpos == std::string::npos) return {};
        auto colon = block.find(':', kpos);
        if (colon == std::string::npos) return {};
        auto quote1 = block.find('"', colon);
        if (quote1 == std::string::npos) return {};
        auto quote2 = block.find('"', quote1 + 1);
        if (quote2 == std::string::npos) return {};
        return block.substr(quote1 + 1, quote2 - quote1 - 1);
    }

    static double extract_json_double(const std::string& block,
                                       const std::string& key) {
        auto kpos = block.find("\"" + key + "\"");
        if (kpos == std::string::npos) return 0;
        auto colon = block.find(':', kpos);
        if (colon == std::string::npos) return 0;
        // Find the number after the colon
        size_t start = colon + 1;
        while (start < block.size() && (block[start] == ' ' || block[start] == '\t'))
            ++start;
        try { return std::stod(block.substr(start)); }
        catch (...) { return 0; }
    }
};

} // namespace straylight::fabric
