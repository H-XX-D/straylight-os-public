/**
 * StrayLight Fabric — Query Engine
 *
 * Rich query interface over the device topology graph.
 * Supports natural-ish device references, bottleneck analysis,
 * NUMA affinity queries, transfer time estimation, and
 * device class queries.
 */
#pragma once

#include "topology.h"
#include "straylight/result.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace straylight::fabric {

// ── Query result types ──────────────────────────────────────────────

struct BottleneckResult {
    std::string from;
    std::string to;
    double bandwidth_gbps = 0.0;
    double latency_ns     = 0.0;
    LinkType link_type    = LinkType::Unknown;
    int hop_index         = 0;  // which hop in the path is the bottleneck
};

struct AffinityResult {
    std::string device_id;
    std::string closest_cpu;
    std::string closest_numa;
    int numa_node         = -1;
    double latency_ns     = 0.0;
};

struct TransferEstimate {
    std::string from;
    std::string to;
    uint64_t bytes                = 0;
    double estimated_time_us      = 0.0;
    double bottleneck_bw_gbps     = 0.0;
    double path_latency_ns        = 0.0;
    int hop_count                 = 0;
};

// ── Query Engine ────────────────────────────────────────────────────

class QueryEngine {
public:
    explicit QueryEngine(Topology& topo) : topo_(topo) {}

    /**
     * Natural-ish query: "fastest path from camera to encoder"
     * Parses device references and finds the optimal path.
     */
    Result<TopologyPath, std::string> query(const std::string& query_str) const {
        // Parse the query to extract source and destination
        auto lower = to_lower(query_str);

        // Pattern: "[fastest|shortest] path from <device> to <device>"
        std::string from_device, to_device;
        bool prefer_bandwidth = false;

        auto from_pos = lower.find("from ");
        auto to_pos = lower.find(" to ");

        if (from_pos != std::string::npos && to_pos != std::string::npos) {
            from_device = query_str.substr(from_pos + 5,
                                           to_pos - (from_pos + 5));
            to_device = query_str.substr(to_pos + 4);
        } else {
            // Try "<device> to <device>" without "from"
            if (to_pos != std::string::npos) {
                from_device = query_str.substr(0, to_pos);
                to_device = query_str.substr(to_pos + 4);
            } else {
                return Result<TopologyPath, std::string>::error(
                    "cannot parse query — expected 'from <device> to <device>'");
            }
        }

        // Trim
        auto trim = [](std::string& s) {
            auto f = s.find_first_not_of(" \t\r\n");
            auto l = s.find_last_not_of(" \t\r\n");
            if (f == std::string::npos) { s.clear(); return; }
            s = s.substr(f, l - f + 1);
        };
        trim(from_device);
        trim(to_device);

        if (lower.find("fastest") != std::string::npos ||
            lower.find("bandwidth") != std::string::npos) {
            prefer_bandwidth = true;
        }

        // Resolve device names to IDs
        auto from_res = resolve_device_ref(from_device);
        if (!from_res) return Result<TopologyPath, std::string>::error(from_res.error());
        auto to_res = resolve_device_ref(to_device);
        if (!to_res) return Result<TopologyPath, std::string>::error(to_res.error());

        if (prefer_bandwidth) {
            return topo_.find_fastest_path(from_res.value(), to_res.value());
        }
        return topo_.find_path(from_res.value(), to_res.value());
    }

    /** Find the bottleneck (slowest link) in a path. */
    Result<BottleneckResult, std::string> get_bottleneck(const TopologyPath& path) const {
        if (path.hops.empty())
            return Result<BottleneckResult, std::string>::error("empty path");

        BottleneckResult result;
        double min_bw = std::numeric_limits<double>::max();
        int idx = 0;

        for (int i = 0; i < static_cast<int>(path.hops.size()); ++i) {
            auto& hop = path.hops[i];
            if (hop.bandwidth_gbps < min_bw) {
                min_bw = hop.bandwidth_gbps;
                result.from = hop.from;
                result.to = hop.to;
                result.bandwidth_gbps = hop.bandwidth_gbps;
                result.latency_ns = hop.latency_ns;
                result.link_type = hop.link_type;
                result.hop_index = i;
                idx = i;
            }
        }

        (void)idx;
        return Result<BottleneckResult, std::string>::ok(std::move(result));
    }

    /** Get NUMA/CPU affinity for a device. */
    Result<AffinityResult, std::string> get_affinity(const std::string& device_ref) const {
        auto dev_res = resolve_device_ref(device_ref);
        if (!dev_res) return Result<AffinityResult, std::string>::error(dev_res.error());

        auto node_res = topo_.get_node(dev_res.value());
        if (!node_res) return Result<AffinityResult, std::string>::error(node_res.error());

        auto& node = node_res.value();
        AffinityResult result;
        result.device_id = node.id;
        result.numa_node = node.numa_node;

        // Find closest CPU via latency path
        auto cpus = topo_.get_all_cpus();
        double best_latency = std::numeric_limits<double>::max();

        for (auto& cpu : cpus) {
            auto path_res = topo_.find_path(node.id, cpu.id);
            if (path_res && path_res.value().total_latency_ns < best_latency) {
                best_latency = path_res.value().total_latency_ns;
                result.closest_cpu = cpu.id;
                result.latency_ns = best_latency;
            }
        }

        // Find closest NUMA node
        if (node.numa_node >= 0) {
            result.closest_numa = "numa:" + std::to_string(node.numa_node);
        } else {
            // Derive from closest CPU
            auto cpu_res = topo_.get_node(result.closest_cpu);
            if (cpu_res) {
                result.closest_numa = "numa:" + std::to_string(cpu_res.value().numa_node);
                result.numa_node = cpu_res.value().numa_node;
            }
        }

        return Result<AffinityResult, std::string>::ok(std::move(result));
    }

    /** Estimate transfer time between two devices. */
    Result<TransferEstimate, std::string> estimate_transfer_time(
            const std::string& from_ref, const std::string& to_ref,
            uint64_t bytes) const {
        auto from_res = resolve_device_ref(from_ref);
        if (!from_res) return Result<TransferEstimate, std::string>::error(from_res.error());
        auto to_res = resolve_device_ref(to_ref);
        if (!to_res) return Result<TransferEstimate, std::string>::error(to_res.error());

        auto path_res = topo_.find_fastest_path(from_res.value(), to_res.value());
        if (!path_res) return Result<TransferEstimate, std::string>::error(path_res.error());

        auto& path = path_res.value();

        TransferEstimate est;
        est.from = from_res.value();
        est.to = to_res.value();
        est.bytes = bytes;
        est.bottleneck_bw_gbps = path.bottleneck_bw_gbps;
        est.path_latency_ns = path.total_latency_ns;
        est.hop_count = static_cast<int>(path.hops.size());

        // Time = latency + data_transfer_time
        // bandwidth in Gbps -> bytes/s = Gbps * 1e9 / 8
        double bw_bytes_per_sec = path.bottleneck_bw_gbps * 1e9 / 8.0;
        double transfer_us = 0;
        if (bw_bytes_per_sec > 0) {
            transfer_us = (static_cast<double>(bytes) / bw_bytes_per_sec) * 1e6;
        }
        double latency_us = path.total_latency_ns / 1000.0;
        est.estimated_time_us = latency_us + transfer_us;

        return Result<TransferEstimate, std::string>::ok(std::move(est));
    }

    /** Get all GPUs. */
    std::vector<DeviceNode> get_all_gpus() const { return topo_.get_all_gpus(); }

    /** Get all storage devices. */
    std::vector<DeviceNode> get_all_storage() const { return topo_.get_all_storage(); }

    /** Get all NICs. */
    std::vector<DeviceNode> get_all_nics() const { return topo_.get_all_nics(); }

    /** Get all CPUs. */
    std::vector<DeviceNode> get_all_cpus() const { return topo_.get_all_cpus(); }

    /** Get devices by type string. */
    std::vector<DeviceNode> get_devices(const std::string& type_str) const {
        auto type = device_type_from_str(type_str);
        if (type != DeviceType::Unknown) return topo_.get_by_type(type);

        // Try fuzzy match across all devices
        auto lower = to_lower(type_str);
        std::vector<DeviceNode> results;
        for (auto& node : topo_.get_all_nodes()) {
            auto name_lower = to_lower(node.name);
            auto id_lower = to_lower(node.id);
            if (name_lower.find(lower) != std::string::npos ||
                id_lower.find(lower) != std::string::npos) {
                results.push_back(node);
            }
        }
        return results;
    }

    /** Get bandwidth between two devices (bottleneck of fastest path). */
    Result<double, std::string> get_bandwidth(const std::string& from_ref,
                                               const std::string& to_ref) const {
        auto from_res = resolve_device_ref(from_ref);
        if (!from_res) return Result<double, std::string>::error(from_res.error());
        auto to_res = resolve_device_ref(to_ref);
        if (!to_res) return Result<double, std::string>::error(to_res.error());

        auto path_res = topo_.find_fastest_path(from_res.value(), to_res.value());
        if (!path_res) return Result<double, std::string>::error(path_res.error());

        return Result<double, std::string>::ok(path_res.value().bottleneck_bw_gbps);
    }

private:
    Topology& topo_;

    /** Resolve a user-provided device reference to a topology node ID. */
    Result<std::string, std::string> resolve_device_ref(const std::string& ref) const {
        // Direct ID match
        auto node_res = topo_.get_node(ref);
        if (node_res) return Result<std::string, std::string>::ok(ref);

        // Type-based match: "gpu", "nvme", "nic", etc.
        auto type = device_type_from_str(to_lower(ref));
        if (type != DeviceType::Unknown) {
            auto devices = topo_.get_by_type(type);
            if (!devices.empty())
                return Result<std::string, std::string>::ok(devices[0].id);
        }

        // Common aliases
        static const std::unordered_map<std::string, std::string> aliases = {
            {"camera",     "usb"},
            {"encoder",    "gpu"},
            {"graphics",   "gpu"},
            {"disk",       "nvme"},
            {"storage",    "nvme"},
            {"network",    "nic"},
            {"ethernet",   "nic"},
            {"wifi",       "nic"},
            {"processor",  "cpu"},
            {"ram",        "memory"},
            {"dram",       "memory"},
        };

        auto lower = to_lower(ref);
        auto alias_it = aliases.find(lower);
        if (alias_it != aliases.end()) {
            auto atype = device_type_from_str(alias_it->second);
            if (atype != DeviceType::Unknown) {
                auto devices = topo_.get_by_type(atype);
                if (!devices.empty())
                    return Result<std::string, std::string>::ok(devices[0].id);
            }
            // Try as ID prefix
            return topo_.resolve_device(alias_it->second);
        }

        // Fuzzy resolve
        return topo_.resolve_device(ref);
    }

    static std::string to_lower(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) out += static_cast<char>(std::tolower(c));
        return out;
    }
};

} // namespace straylight::fabric
