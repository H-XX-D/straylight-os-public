// services/weave/pipeline_engine.h
// Service wiring — create, start, stop, and monitor data pipelines.
#pragma once

#include "node_registry.h"
#include "pipeline_parser.h"

#include <straylight/result.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace straylight {

/// Runtime state of a pipeline node.
struct NodeRuntime {
    std::string instance_name;
    std::string node_type;
    pid_t pid = 0;
    int splice_fd_in = -1;   // splice(2) fd for zero-copy input
    int splice_fd_out = -1;  // splice(2) fd for zero-copy output
    uint64_t bytes_processed = 0;
    uint64_t messages_processed = 0;
    std::chrono::steady_clock::time_point start_time;
};

/// Per-node metrics.
struct NodeMetrics {
    std::string instance_name;
    std::string node_type;
    double throughput_mbps;
    double latency_ms;
    uint64_t messages_processed;
    uint64_t bytes_processed;
    bool is_bottleneck;
};

/// Pipeline runtime state.
enum class PipelineState {
    Created,
    Starting,
    Running,
    Stopping,
    Stopped,
    Error
};

/// A running pipeline with its nodes and connections.
struct PipelineRuntime {
    PipelineDefinition definition;
    PipelineState state = PipelineState::Created;
    std::vector<NodeRuntime> nodes;
    std::chrono::steady_clock::time_point start_time;
    std::string error_message;
};

/// Engine that manages pipeline lifecycle — create, start, stop, monitor.
class PipelineEngine {
public:
    PipelineEngine();

    /// Create a pipeline from a DSL spec string.
    Result<void, std::string> create_from_dsl(const std::string& name,
                                               const std::string& spec);

    /// Create a pipeline from a JSON definition.
    Result<void, std::string> create_from_json(const std::string& name,
                                                const nlohmann::json& def);

    /// Start a created pipeline — activate all nodes and begin data flow.
    Result<void, std::string> start_pipeline(const std::string& name);

    /// Stop a running pipeline — graceful shutdown in reverse order.
    Result<void, std::string> stop_pipeline(const std::string& name);

    /// Delete a pipeline (must be stopped first).
    Result<void, std::string> delete_pipeline(const std::string& name);

    /// Get metrics for a pipeline — throughput, latency per node, bottlenecks.
    Result<std::vector<NodeMetrics>, std::string> get_metrics(const std::string& name);

    /// List all pipelines with their states.
    std::vector<std::pair<std::string, PipelineState>> list_pipelines() const;

    /// Get detailed status of a pipeline.
    Result<nlohmann::json, std::string> get_status(const std::string& name) const;

    /// Get the node registry (for listing available node types).
    const NodeRegistry& registry() const { return registry_; }

private:
    /// Launch a single node process.
    Result<NodeRuntime, std::string> launch_node(const PipelineNode& node);

    /// Set up splice(2) zero-copy between adjacent nodes.
    Result<void, std::string> setup_splice(NodeRuntime& upstream, NodeRuntime& downstream);

    /// Stop a single node process.
    void stop_node(NodeRuntime& node);

    /// Read runtime metrics from a node process.
    NodeMetrics read_node_metrics(const NodeRuntime& node) const;

    NodeRegistry registry_;
    PipelineParser parser_;

    mutable std::mutex mu_;
    std::map<std::string, PipelineRuntime> pipelines_;
};

} // namespace straylight
