/**
 * StrayLight Sync Kernel — SyncGraph (Dependency DAG)
 *
 * Directed acyclic graph of sync dependencies — the orchestration
 * layer on top of fences and timelines. Inspired by NVIDIA's
 * command buffer dependency graphs and Vulkan's render pass
 * subpass dependencies.
 *
 * Use cases:
 *   - Boot ordering: define service startup DAG, execute in parallel
 *   - Compositor pipeline: render-pass → post-process → present
 *   - Package install: dependency-ordered build graph
 *
 * The graph detects cycles at insertion time, computes topological
 * order, and executes nodes in parallel waves (like GPU warp scheduling).
 */
#pragma once

#include "fence.h"
#include "timeline.h"
#include <straylight/result.h>

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace straylight::sync {

/// A node in the dependency graph.
struct SyncNode {
    std::string id;                     // Unique identifier
    std::string label;                  // Human-readable name
    std::vector<std::string> depends_on; // IDs this node waits for
    int priority = 0;                   // Higher = scheduled first within a wave

    /// User-supplied work function. Called when all dependencies are satisfied.
    /// Return true = success, false = failed.
    std::function<bool()> execute;

    /// Optional estimated duration (for scheduling hints).
    int estimated_ms = 0;
};

/// Execution state of a node.
enum class NodeState : uint8_t {
    Pending,    // Not yet started
    Waiting,    // Dependencies not met
    Running,    // Currently executing
    Completed,  // Finished successfully
    Failed,     // Finished with error
    Skipped     // Skipped due to failed dependency
};

inline const char* node_state_str(NodeState s) {
    switch (s) {
        case NodeState::Pending:   return "pending";
        case NodeState::Waiting:   return "waiting";
        case NodeState::Running:   return "running";
        case NodeState::Completed: return "completed";
        case NodeState::Failed:    return "failed";
        case NodeState::Skipped:   return "skipped";
    }
    return "unknown";
}

/// Runtime info about a node during/after execution.
struct NodeStatus {
    std::string id;
    NodeState state = NodeState::Pending;
    int wave = -1;                   // Which parallel wave this belongs to
    double elapsed_ms = 0.0;
    std::string error_msg;
};

/// A wave — a set of nodes that can run in parallel.
struct Wave {
    int index;
    std::vector<std::string> node_ids;
};

/// Graph execution options.
struct GraphExecOptions {
    int max_parallel = 0;           // 0 = unlimited parallelism within a wave
    bool fail_fast = true;          // Stop on first failure
    bool skip_on_dep_failure = true; // Skip nodes whose deps failed
    int node_timeout_ms = -1;       // Per-node timeout
    std::function<void(const NodeStatus&)> on_status_change;  // Progress callback
};

/// Execution result.
struct GraphExecResult {
    bool success = true;
    int total_nodes = 0;
    int completed = 0;
    int failed = 0;
    int skipped = 0;
    double total_ms = 0.0;
    std::vector<NodeStatus> node_statuses;
};

/// A dependency graph with topological sort and parallel wave execution.
class SyncGraph {
public:
    SyncGraph() = default;
    explicit SyncGraph(const std::string& name) : name_(name) {}

    /// Add a node to the graph.
    VoidResult<std::string> add_node(SyncNode node);

    /// Remove a node (and all edges referencing it).
    VoidResult<std::string> remove_node(const std::string& id);

    /// Add a dependency edge: `from` must complete before `to` starts.
    VoidResult<std::string> add_edge(const std::string& from, const std::string& to);

    /// Check for cycles. Returns the cycle path if one exists.
    Result<std::vector<std::string>, std::string> detect_cycle() const;

    /// Compute topological sort. Fails if graph has cycles.
    Result<std::vector<std::string>, std::string> topological_sort() const;

    /// Compute parallel execution waves — nodes in each wave have no
    /// mutual dependencies and can run concurrently.
    Result<std::vector<Wave>, std::string> compute_waves() const;

    /// Execute the graph — runs waves in order, nodes within a wave
    /// in parallel. This is the main scheduler entry point.
    Result<GraphExecResult, std::string> execute(const GraphExecOptions& opts = {});

    /// Get all nodes.
    [[nodiscard]] const std::map<std::string, SyncNode>& nodes() const { return nodes_; }

    /// Get node count.
    [[nodiscard]] size_t size() const { return nodes_.size(); }

    /// Get name.
    [[nodiscard]] const std::string& name() const { return name_; }

    /// DOT format export for visualization.
    std::string to_dot() const;

private:
    std::string name_;
    std::map<std::string, SyncNode> nodes_;
    mutable std::mutex exec_mutex_;

    /// DFS cycle detection helper.
    bool has_cycle_from(const std::string& node,
                        std::set<std::string>& visited,
                        std::set<std::string>& in_stack,
                        std::vector<std::string>& path) const;
};

} // namespace straylight::sync
