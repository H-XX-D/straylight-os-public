// services/lens/correlator.cpp
// Causal event correlation engine for full-stack trace analysis.

#include "correlator.h"

#include <straylight/log.h>

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace straylight {

// Known causal patterns between layers.
// If event A is of type X in layer L1 and event B is of type Y in layer L2,
// and B starts shortly after A ends, we consider A->B causal.
struct CausalPattern {
    TraceLayer from_layer;
    std::string from_type_prefix;
    TraceLayer to_layer;
    std::string to_type_prefix;
    std::string relationship;
};

static const std::vector<CausalPattern> CAUSAL_PATTERNS = {
    // Mouse click in compositor -> app receives input event
    {TraceLayer::Compositor, "mouse_",     TraceLayer::App,        "input_",      "input_dispatch"},
    {TraceLayer::Compositor, "key_",       TraceLayer::App,        "input_",      "input_dispatch"},
    // Compositor dispatch -> app receive
    {TraceLayer::Compositor, "dispatch",   TraceLayer::App,        "recv",        "dispatch"},
    {TraceLayer::Compositor, "compositor", TraceLayer::Ipc,        "ipc_",        "compositor_ipc"},
    // App -> IPC send
    {TraceLayer::App,        "render_",    TraceLayer::Vpu,        "vpu_alloc",   "vpu_request"},
    {TraceLayer::App,        "submit",     TraceLayer::Gpu,        "gpu_",        "gpu_submit"},
    // IPC message flow
    {TraceLayer::Ipc,        "ipc_send",   TraceLayer::Ipc,        "ipc_recv",    "ipc_transfer"},
    {TraceLayer::Ipc,        "ipc_",       TraceLayer::App,        "recv",        "ipc_delivery"},
    // VPU allocation -> GPU render
    {TraceLayer::Vpu,        "vpu_alloc",  TraceLayer::Gpu,        "gpu_render",  "vpu_to_gpu"},
    // GPU render -> compositor present
    {TraceLayer::Gpu,        "gpu_render", TraceLayer::Compositor, "present",     "present"},
    {TraceLayer::Gpu,        "gpu_",       TraceLayer::Compositor, "flip",        "scanout"},
    // Kernel scheduling
    {TraceLayer::Kernel,     "sched_",     TraceLayer::App,        "",            "schedule"},
};

bool Correlator::could_cause(const TraceEvent& a, const TraceEvent& b) const {
    // A must end before or at the same time B starts
    uint64_t a_end = a.timestamp_ns + a.duration_ns;
    if (b.timestamp_ns < a.timestamp_ns) return false;

    // Gap between A's end and B's start must be within threshold
    uint64_t gap = (b.timestamp_ns > a_end) ? (b.timestamp_ns - a_end) : 0;
    return gap <= max_gap_ns_;
}

bool Correlator::has_causal_pattern(const TraceEvent& a, const TraceEvent& b) {
    for (const auto& pat : CAUSAL_PATTERNS) {
        if (a.layer == pat.from_layer && b.layer == pat.to_layer) {
            bool from_match = pat.from_type_prefix.empty() ||
                              a.event_type.find(pat.from_type_prefix) != std::string::npos;
            bool to_match = pat.to_type_prefix.empty() ||
                            b.event_type.find(pat.to_type_prefix) != std::string::npos;
            if (from_match && to_match) return true;
        }
    }
    return false;
}

CausalGraph Correlator::correlate(const Trace& trace) const {
    CausalGraph graph;
    graph.events = trace.events;

    // Sort by timestamp
    std::sort(graph.events.begin(), graph.events.end());

    size_t n = graph.events.size();
    if (n == 0) return graph;

    // Build adjacency: for each event, find subsequent events it could have caused
    std::unordered_set<size_t> has_incoming;
    std::unordered_set<size_t> has_outgoing;

    for (size_t i = 0; i < n; ++i) {
        const auto& ev_a = graph.events[i];

        for (size_t j = i + 1; j < n; ++j) {
            const auto& ev_b = graph.events[j];

            // Stop scanning once events are too far in the future
            if (ev_b.timestamp_ns > ev_a.timestamp_ns + ev_a.duration_ns + max_gap_ns_) {
                break;
            }

            // Skip events in the same layer (parallel events, not causal)
            if (ev_a.layer == ev_b.layer && ev_a.event_type == ev_b.event_type) {
                continue;
            }

            if (could_cause(ev_a, ev_b) && has_causal_pattern(ev_a, ev_b)) {
                CausalEdge edge;
                edge.from_idx = i;
                edge.to_idx = j;

                // Find the relationship name
                for (const auto& pat : CAUSAL_PATTERNS) {
                    if (ev_a.layer == pat.from_layer && ev_b.layer == pat.to_layer) {
                        bool from_match = pat.from_type_prefix.empty() ||
                                          ev_a.event_type.find(pat.from_type_prefix) != std::string::npos;
                        bool to_match = pat.to_type_prefix.empty() ||
                                        ev_b.event_type.find(pat.to_type_prefix) != std::string::npos;
                        if (from_match && to_match) {
                            edge.relationship = pat.relationship;
                            break;
                        }
                    }
                }

                uint64_t a_end = ev_a.timestamp_ns + ev_a.duration_ns;
                edge.gap_ns = (ev_b.timestamp_ns > a_end) ? (ev_b.timestamp_ns - a_end) : 0;

                graph.edges.push_back(edge);
                has_incoming.insert(j);
                has_outgoing.insert(i);
            }
        }
    }

    // Identify roots and leaves
    for (size_t i = 0; i < n; ++i) {
        if (has_incoming.find(i) == has_incoming.end()) {
            graph.roots.push_back(i);
        }
        if (has_outgoing.find(i) == has_outgoing.end()) {
            graph.leaves.push_back(i);
        }
    }

    return graph;
}

void Correlator::longest_path_dfs(const CausalGraph& graph,
                                    size_t node,
                                    uint64_t current_duration,
                                    std::vector<size_t>& current_path,
                                    CriticalPath& best) const {
    current_path.push_back(node);
    uint64_t node_duration = graph.events[node].duration_ns;
    current_duration += node_duration;

    // Find outgoing edges from this node
    bool has_children = false;
    for (const auto& edge : graph.edges) {
        if (edge.from_idx == node) {
            has_children = true;
            longest_path_dfs(graph, edge.to_idx,
                             current_duration + edge.gap_ns,
                             current_path, best);
        }
    }

    if (!has_children) {
        // Leaf node — check if this is the longest path
        if (current_duration > best.total_duration_ns) {
            best.event_indices = current_path;
            best.total_duration_ns = current_duration;

            // Compute gap vs work time
            best.total_work_ns = 0;
            best.total_gap_ns = 0;
            for (size_t idx : current_path) {
                best.total_work_ns += graph.events[idx].duration_ns;
            }
            best.total_gap_ns = current_duration - best.total_work_ns;
        }
    }

    current_path.pop_back();
}

CriticalPath Correlator::get_critical_path(const CausalGraph& graph) const {
    CriticalPath best;
    std::vector<size_t> current_path;

    // Start DFS from each root
    for (size_t root : graph.roots) {
        longest_path_dfs(graph, root, 0, current_path, best);
    }

    // If no roots found (no edges), just find the longest single event
    if (best.event_indices.empty() && !graph.events.empty()) {
        size_t max_idx = 0;
        for (size_t i = 1; i < graph.events.size(); ++i) {
            if (graph.events[i].duration_ns > graph.events[max_idx].duration_ns) {
                max_idx = i;
            }
        }
        best.event_indices.push_back(max_idx);
        best.total_duration_ns = graph.events[max_idx].duration_ns;
        best.total_work_ns = best.total_duration_ns;
    }

    return best;
}

std::string Correlator::suggest_optimization(const TraceEvent& event) {
    switch (event.layer) {
        case TraceLayer::Compositor:
            if (event.event_type.find("present") != std::string::npos) {
                return "Consider reducing compositor overhead via direct scanout bypass";
            }
            if (event.event_type.find("dispatch") != std::string::npos) {
                return "Input dispatch latency high; check compositor event queue depth";
            }
            return "Desktop presentation is the bottleneck; profile the GNOME/Mutter session and GPU path";

        case TraceLayer::Ipc:
            return "IPC transfer is the bottleneck; consider using splice for zero-copy";

        case TraceLayer::Vpu:
            if (event.event_type.find("alloc") != std::string::npos) {
                return "VPU allocation is slow; check slab fragmentation or increase pool size";
            }
            return "VPU operation is the bottleneck; check VPU utilization with straylight-autotune";

        case TraceLayer::Gpu:
            if (event.event_type.find("render") != std::string::npos) {
                return "GPU render is the bottleneck; check shader complexity and batch count";
            }
            return "GPU is the bottleneck; consider offloading to VPU or reducing render load";

        case TraceLayer::App:
            return "Application logic is the bottleneck; profile the app with straylight-lens --deep";

        case TraceLayer::Kernel:
            if (event.event_type.find("sched") != std::string::npos) {
                return "Scheduling latency is high; check CPU affinity and real-time priority";
            }
            return "Kernel overhead is the bottleneck; check for lock contention in dmesg";

        case TraceLayer::Network:
            return "Network is the bottleneck; check bandwidth and latency with straylight-mesh stats";
    }

    return "Profile this step for optimization opportunities";
}

Bottleneck Correlator::get_bottleneck(const CausalGraph& graph) const {
    CriticalPath cp = get_critical_path(graph);

    if (cp.event_indices.empty()) {
        return Bottleneck{};
    }

    // Find the event on the critical path with the longest duration
    size_t worst_idx = cp.event_indices[0];
    uint64_t worst_duration = graph.events[worst_idx].duration_ns;

    for (size_t i = 1; i < cp.event_indices.size(); ++i) {
        size_t idx = cp.event_indices[i];
        if (graph.events[idx].duration_ns > worst_duration) {
            worst_idx = idx;
            worst_duration = graph.events[idx].duration_ns;
        }
    }

    Bottleneck bn;
    bn.event_idx = worst_idx;
    bn.event = graph.events[worst_idx];
    bn.duration_ns = worst_duration;
    bn.fraction = (cp.total_duration_ns > 0)
        ? static_cast<double>(worst_duration) / static_cast<double>(cp.total_duration_ns)
        : 1.0;
    bn.suggestion = suggest_optimization(bn.event);

    return bn;
}

} // namespace straylight
