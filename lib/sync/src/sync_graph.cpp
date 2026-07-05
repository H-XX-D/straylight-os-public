/**
 * StrayLight Sync Kernel — SyncGraph implementation
 *
 * DAG-based dependency graph with cycle detection, topological sort,
 * and parallel wave execution. This is the orchestration brain —
 * like NVIDIA's command buffer scheduler that figures out which
 * GPU workloads can run in parallel based on their data dependencies.
 */
#include "straylight/sync/sync_graph.h"

#include <algorithm>
#include <chrono>
#include <queue>
#include <sstream>
#include <thread>

namespace straylight::sync {

// ── Node management ─────────────────────────────────────────────

VoidResult<std::string> SyncGraph::add_node(SyncNode node) {
    if (node.id.empty()) {
        return VoidResult<std::string>::error("Node ID cannot be empty");
    }
    if (nodes_.count(node.id)) {
        return VoidResult<std::string>::error("Node '" + node.id + "' already exists");
    }

    // Verify all dependencies exist (or will be added later — relaxed mode)
    // We check at execution time instead.

    nodes_[node.id] = std::move(node);
    return VoidResult<std::string>::ok();
}

VoidResult<std::string> SyncGraph::remove_node(const std::string& id) {
    auto it = nodes_.find(id);
    if (it == nodes_.end()) {
        return VoidResult<std::string>::error("Node '" + id + "' not found");
    }

    // Remove this node from all other nodes' dependency lists
    for (auto& [nid, node] : nodes_) {
        auto& deps = node.depends_on;
        deps.erase(std::remove(deps.begin(), deps.end(), id), deps.end());
    }

    nodes_.erase(it);
    return VoidResult<std::string>::ok();
}

VoidResult<std::string> SyncGraph::add_edge(const std::string& from, const std::string& to) {
    if (!nodes_.count(to)) {
        return VoidResult<std::string>::error("Target node '" + to + "' not found");
    }

    auto& deps = nodes_[to].depends_on;
    if (std::find(deps.begin(), deps.end(), from) == deps.end()) {
        deps.push_back(from);
    }

    // Quick cycle check
    auto cycle = detect_cycle();
    if (cycle.has_value() && !cycle.value().empty()) {
        // Remove the edge we just added to keep graph valid
        deps.erase(std::remove(deps.begin(), deps.end(), from), deps.end());
        return VoidResult<std::string>::error("Adding edge " + from + " → " + to
            + " would create a cycle");
    }

    return VoidResult<std::string>::ok();
}

// ── Cycle detection ─────────────────────────────────────────────

bool SyncGraph::has_cycle_from(const std::string& node,
                                 std::set<std::string>& visited,
                                 std::set<std::string>& in_stack,
                                 std::vector<std::string>& path) const
{
    visited.insert(node);
    in_stack.insert(node);
    path.push_back(node);

    auto it = nodes_.find(node);
    if (it != nodes_.end()) {
        for (const auto& dep : it->second.depends_on) {
            if (!visited.count(dep)) {
                if (has_cycle_from(dep, visited, in_stack, path)) {
                    return true;
                }
            } else if (in_stack.count(dep)) {
                path.push_back(dep);
                return true;
            }
        }
    }

    in_stack.erase(node);
    path.pop_back();
    return false;
}

Result<std::vector<std::string>, std::string> SyncGraph::detect_cycle() const {
    std::set<std::string> visited;
    std::set<std::string> in_stack;
    std::vector<std::string> path;

    for (const auto& [id, _] : nodes_) {
        if (!visited.count(id)) {
            if (has_cycle_from(id, visited, in_stack, path)) {
                return Result<std::vector<std::string>, std::string>::ok(std::move(path));
            }
        }
    }

    // Empty vector = no cycle
    return Result<std::vector<std::string>, std::string>::ok(std::vector<std::string>{});
}

// ── Topological sort (Kahn's algorithm) ─────────────────────────

Result<std::vector<std::string>, std::string> SyncGraph::topological_sort() const {
    // Build in-degree map
    std::map<std::string, int> in_degree;
    std::map<std::string, std::vector<std::string>> reverse_deps; // who depends on me

    for (const auto& [id, _] : nodes_) {
        in_degree[id] = 0;
    }

    for (const auto& [id, node] : nodes_) {
        for (const auto& dep : node.depends_on) {
            if (nodes_.count(dep)) {
                in_degree[id]++;
                reverse_deps[dep].push_back(id);
            }
        }
    }

    // Priority queue: nodes with 0 in-degree, sorted by priority
    auto cmp = [this](const std::string& a, const std::string& b) {
        return nodes_.at(a).priority < nodes_.at(b).priority;
    };
    std::priority_queue<std::string, std::vector<std::string>, decltype(cmp)> ready(cmp);

    for (const auto& [id, deg] : in_degree) {
        if (deg == 0) ready.push(id);
    }

    std::vector<std::string> sorted;
    sorted.reserve(nodes_.size());

    while (!ready.empty()) {
        auto id = ready.top();
        ready.pop();
        sorted.push_back(id);

        for (const auto& dependent : reverse_deps[id]) {
            if (--in_degree[dependent] == 0) {
                ready.push(dependent);
            }
        }
    }

    if (sorted.size() != nodes_.size()) {
        return Result<std::vector<std::string>, std::string>::error(
            "Graph has a cycle — topological sort impossible");
    }

    return Result<std::vector<std::string>, std::string>::ok(std::move(sorted));
}

// ── Wave computation ────────────────────────────────────────────

Result<std::vector<Wave>, std::string> SyncGraph::compute_waves() const {
    // Assign each node to a wave = max depth from any root
    std::map<std::string, int> depth;

    // Get topological order first
    auto topo = topological_sort();
    if (!topo.has_value()) {
        return Result<std::vector<Wave>, std::string>::error(topo.error());
    }

    // Compute max depth for each node
    for (const auto& id : topo.value()) {
        int max_dep_depth = -1;
        for (const auto& dep : nodes_.at(id).depends_on) {
            if (depth.count(dep)) {
                max_dep_depth = std::max(max_dep_depth, depth[dep]);
            }
        }
        depth[id] = max_dep_depth + 1;
    }

    // Group by wave
    int max_wave = 0;
    for (const auto& [_, d] : depth) {
        max_wave = std::max(max_wave, d);
    }

    std::vector<Wave> waves(max_wave + 1);
    for (int i = 0; i <= max_wave; ++i) {
        waves[i].index = i;
    }

    for (const auto& [id, d] : depth) {
        waves[d].node_ids.push_back(id);
    }

    // Sort within each wave by priority
    for (auto& wave : waves) {
        std::sort(wave.node_ids.begin(), wave.node_ids.end(),
            [this](const std::string& a, const std::string& b) {
                return nodes_.at(a).priority > nodes_.at(b).priority;
            });
    }

    return Result<std::vector<Wave>, std::string>::ok(std::move(waves));
}

// ── Execute ─────────────────────────────────────────────────────

Result<GraphExecResult, std::string> SyncGraph::execute(const GraphExecOptions& opts) {
    std::lock_guard<std::mutex> exec_lock(exec_mutex_);

    auto waves_result = compute_waves();
    if (!waves_result.has_value()) {
        return Result<GraphExecResult, std::string>::error(waves_result.error());
    }

    auto start = std::chrono::steady_clock::now();
    const auto& waves = waves_result.value();

    GraphExecResult result;
    result.total_nodes = static_cast<int>(nodes_.size());

    std::map<std::string, NodeStatus> statuses;
    for (const auto& [id, _] : nodes_) {
        statuses[id] = NodeStatus{id, NodeState::Pending};
    }

    bool abort = false;

    for (const auto& wave : waves) {
        if (abort) break;

        // Mark nodes in this wave as waiting
        for (const auto& id : wave.node_ids) {
            statuses[id].state = NodeState::Waiting;
            statuses[id].wave = wave.index;
            if (opts.on_status_change) opts.on_status_change(statuses[id]);
        }

        // Check for failed dependencies
        std::vector<std::string> runnable;
        for (const auto& id : wave.node_ids) {
            bool deps_ok = true;
            for (const auto& dep : nodes_[id].depends_on) {
                if (statuses.count(dep) && statuses[dep].state == NodeState::Failed) {
                    deps_ok = false;
                    break;
                }
            }

            if (!deps_ok && opts.skip_on_dep_failure) {
                statuses[id].state = NodeState::Skipped;
                statuses[id].error_msg = "Dependency failed";
                result.skipped++;
                if (opts.on_status_change) opts.on_status_change(statuses[id]);
            } else {
                runnable.push_back(id);
            }
        }

        // Execute runnable nodes in parallel
        int max_par = opts.max_parallel > 0
            ? std::min(opts.max_parallel, static_cast<int>(runnable.size()))
            : static_cast<int>(runnable.size());

        std::vector<std::thread> threads;
        std::mutex result_mutex;

        for (int i = 0; i < static_cast<int>(runnable.size()); ++i) {
            // If we've hit parallelism limit, wait for a slot
            while (static_cast<int>(threads.size()) >= max_par) {
                threads.front().join();
                threads.erase(threads.begin());
            }

            const auto& id = runnable[i];
            threads.emplace_back([&, id]() {
                auto node_start = std::chrono::steady_clock::now();

                {
                    std::lock_guard<std::mutex> lock(result_mutex);
                    statuses[id].state = NodeState::Running;
                    if (opts.on_status_change) opts.on_status_change(statuses[id]);
                }

                bool success = false;
                if (nodes_[id].execute) {
                    success = nodes_[id].execute();
                } else {
                    success = true; // No-op node
                }

                auto elapsed = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - node_start).count();

                std::lock_guard<std::mutex> lock(result_mutex);
                statuses[id].elapsed_ms = elapsed;

                if (success) {
                    statuses[id].state = NodeState::Completed;
                    result.completed++;
                } else {
                    statuses[id].state = NodeState::Failed;
                    statuses[id].error_msg = "Execution failed";
                    result.failed++;
                    result.success = false;
                    if (opts.fail_fast) abort = true;
                }

                if (opts.on_status_change) opts.on_status_change(statuses[id]);
            });
        }

        // Wait for all threads in this wave
        for (auto& t : threads) {
            if (t.joinable()) t.join();
        }
    }

    result.total_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();

    for (auto& [id, status] : statuses) {
        result.node_statuses.push_back(std::move(status));
    }

    return Result<GraphExecResult, std::string>::ok(std::move(result));
}

// ── DOT export ──────────────────────────────────────────────────

std::string SyncGraph::to_dot() const {
    std::ostringstream out;
    out << "digraph \"" << name_ << "\" {\n";
    out << "  rankdir=LR;\n";
    out << "  node [shape=box, style=filled, fillcolor=\"#2a2a4e\", "
        << "fontcolor=\"#00ff88\", color=\"#00ff88\"];\n";
    out << "  edge [color=\"#00ff88\"];\n\n";

    for (const auto& [id, node] : nodes_) {
        out << "  \"" << id << "\" [label=\"" << node.label << "\"];\n";
        for (const auto& dep : node.depends_on) {
            out << "  \"" << dep << "\" -> \"" << id << "\";\n";
        }
    }

    out << "}\n";
    return out.str();
}

} // namespace straylight::sync
