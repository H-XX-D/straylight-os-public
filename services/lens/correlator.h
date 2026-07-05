// services/lens/correlator.h
// Event correlation engine — builds causal DAGs from trace events.
#pragma once

#include "trace_collector.h"

#include <straylight/result.h>
#include <straylight/error.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace straylight {

/// An edge in the causal DAG: from one event to the next in the chain.
struct CausalEdge {
    size_t from_idx{0};       // Index into the Trace::events vector
    size_t to_idx{0};
    std::string relationship;  // e.g., "dispatch", "ipc_send", "alloc", "render"
    uint64_t gap_ns{0};       // Time between from.end and to.start
};

/// The causal DAG: events as nodes, edges as causal relationships.
struct CausalGraph {
    std::vector<TraceEvent> events;    // All events (sorted by timestamp)
    std::vector<CausalEdge> edges;     // Causal edges
    std::vector<size_t> roots;         // Events with no incoming edges (initiators)
    std::vector<size_t> leaves;        // Events with no outgoing edges (terminals)
};

/// The critical path: the longest dependency chain in the DAG.
struct CriticalPath {
    std::vector<size_t> event_indices;  // Indices into CausalGraph::events
    uint64_t total_duration_ns{0};      // Sum of all event durations + gaps
    uint64_t total_gap_ns{0};           // Time spent waiting between events
    uint64_t total_work_ns{0};          // Time spent in events
};

/// Bottleneck information.
struct Bottleneck {
    size_t event_idx{0};               // Index of the bottleneck event
    TraceEvent event;                   // The bottleneck event itself
    uint64_t duration_ns{0};           // How long the bottleneck took
    double fraction{0.0};             // Fraction of total critical path time
    std::string suggestion;            // Human-readable optimization suggestion
};

/// Correlates trace events across layers by timing proximity and causal relationships.
class Correlator {
public:
    /// Correlate events from a trace into a causal DAG.
    /// Matches events across layers by:
    ///   1. Same correlation_id
    ///   2. Timing proximity (event B starts shortly after event A ends)
    ///   3. Causal patterns (e.g., compositor dispatch -> app receive)
    CausalGraph correlate(const Trace& trace) const;

    /// Find the critical path (longest dependency chain = user-perceived latency).
    CriticalPath get_critical_path(const CausalGraph& graph) const;

    /// Find the bottleneck (slowest step on the critical path).
    Bottleneck get_bottleneck(const CausalGraph& graph) const;

    /// Set the maximum time gap (ns) to consider two events causally related.
    void set_max_gap_ns(uint64_t gap) { max_gap_ns_ = gap; }

private:
    /// Check if event A could have caused event B based on timing.
    bool could_cause(const TraceEvent& a, const TraceEvent& b) const;

    /// Check if two events have a known causal pattern.
    static bool has_causal_pattern(const TraceEvent& a, const TraceEvent& b);

    /// Compute the longest path from a node in the DAG using DFS.
    void longest_path_dfs(const CausalGraph& graph,
                          size_t node,
                          uint64_t current_duration,
                          std::vector<size_t>& current_path,
                          CriticalPath& best) const;

    /// Generate an optimization suggestion for a bottleneck event.
    static std::string suggest_optimization(const TraceEvent& event);

    uint64_t max_gap_ns_{5000000}; // 5ms default max gap
};

} // namespace straylight
