// services/weave/pipeline_parser.h
// Parse pipeline definitions from DSL strings and JSON.
#pragma once

#include "node_registry.h"

#include <straylight/result.h>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace straylight {

/// A single node instance in a pipeline with its configuration.
struct PipelineNode {
    std::string node_type;     // References a NodeType name
    std::string instance_name; // Unique name within the pipeline
    nlohmann::json config;     // Node-specific configuration
};

/// A connection between two pipeline nodes.
struct PipelineConnection {
    std::string from_node;
    std::string to_node;
};

/// A parsed pipeline definition ready for execution.
struct PipelineDefinition {
    std::string name;
    std::vector<PipelineNode> nodes;
    std::vector<PipelineConnection> connections;
};

/// Parses pipeline definitions from DSL strings and JSON.
class PipelineParser {
public:
    explicit PipelineParser(const NodeRegistry& registry);

    /// Parse a DSL pipeline spec:
    ///   "camera-capture | vpu-encode --codec=h265 | mesh-broadcast --group=renders"
    Result<PipelineDefinition, std::string> parse_dsl(const std::string& name,
                                                       const std::string& spec) const;

    /// Parse a JSON pipeline definition (supports branches).
    Result<PipelineDefinition, std::string> parse_json(const std::string& name,
                                                        const nlohmann::json& j) const;

    /// Validate a pipeline definition — check types, connections, cycles.
    Result<void, std::string> validate(const PipelineDefinition& pipeline) const;

private:
    /// Parse a single node spec from DSL: "vpu-encode --codec=h265 --bitrate=5000000"
    Result<PipelineNode, std::string> parse_node_spec(const std::string& spec,
                                                       int index) const;

    /// Parse --key=value arguments into a JSON config object.
    static nlohmann::json parse_args(const std::vector<std::string>& args);

    const NodeRegistry& registry_;
};

} // namespace straylight
