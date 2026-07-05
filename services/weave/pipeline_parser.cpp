// services/weave/pipeline_parser.cpp
#include "pipeline_parser.h"

#include <algorithm>
#include <set>
#include <sstream>

namespace straylight {

PipelineParser::PipelineParser(const NodeRegistry& registry)
    : registry_(registry) {}

nlohmann::json PipelineParser::parse_args(const std::vector<std::string>& args) {
    nlohmann::json config;

    for (const auto& arg : args) {
        if (arg.rfind("--", 0) != 0) continue;

        std::string kv = arg.substr(2);
        auto eq = kv.find('=');

        std::string key;
        std::string value;

        if (eq != std::string::npos) {
            key = kv.substr(0, eq);
            value = kv.substr(eq + 1);
        } else {
            // Boolean flag
            key = kv;
            config[key] = true;
            continue;
        }

        // Try to parse as number
        try {
            size_t pos = 0;
            int64_t int_val = std::stoll(value, &pos);
            if (pos == value.size()) {
                config[key] = int_val;
                continue;
            }
        } catch (...) {}

        try {
            size_t pos = 0;
            double dbl_val = std::stod(value, &pos);
            if (pos == value.size()) {
                config[key] = dbl_val;
                continue;
            }
        } catch (...) {}

        // Boolean strings
        if (value == "true") { config[key] = true; continue; }
        if (value == "false") { config[key] = false; continue; }

        // Default: string
        config[key] = value;
    }

    return config;
}

Result<PipelineNode, std::string> PipelineParser::parse_node_spec(
    const std::string& spec, int index) const {
    // Tokenize on whitespace
    std::istringstream iss(spec);
    std::vector<std::string> tokens;
    std::string token;
    while (iss >> token) {
        tokens.push_back(token);
    }

    if (tokens.empty()) {
        return Result<PipelineNode, std::string>::error(
            "Empty node spec at position " + std::to_string(index));
    }

    std::string node_type = tokens[0];

    // Verify node type exists
    if (!registry_.has(node_type)) {
        return Result<PipelineNode, std::string>::error(
            "Unknown node type '" + node_type + "' at position " + std::to_string(index));
    }

    PipelineNode node;
    node.node_type = node_type;
    node.instance_name = node_type + "-" + std::to_string(index);

    // Parse --key=value args
    std::vector<std::string> args(tokens.begin() + 1, tokens.end());
    node.config = parse_args(args);

    return Result<PipelineNode, std::string>::ok(std::move(node));
}

Result<PipelineDefinition, std::string> PipelineParser::parse_dsl(
    const std::string& name, const std::string& spec) const {
    PipelineDefinition pipeline;
    pipeline.name = name;

    // Split on '|'
    std::vector<std::string> segments;
    std::string current;

    for (size_t i = 0; i < spec.size(); ++i) {
        if (spec[i] == '|') {
            // Trim
            auto start = current.find_first_not_of(' ');
            auto end = current.find_last_not_of(' ');
            if (start != std::string::npos) {
                segments.push_back(current.substr(start, end - start + 1));
            }
            current.clear();
        } else {
            current += spec[i];
        }
    }

    // Last segment
    auto start = current.find_first_not_of(' ');
    auto end = current.find_last_not_of(' ');
    if (start != std::string::npos) {
        segments.push_back(current.substr(start, end - start + 1));
    }

    if (segments.empty()) {
        return Result<PipelineDefinition, std::string>::error(
            "Empty pipeline spec");
    }

    // Parse each segment as a node
    for (size_t i = 0; i < segments.size(); ++i) {
        auto node_result = parse_node_spec(segments[i], static_cast<int>(i));
        if (!node_result.has_value()) {
            return Result<PipelineDefinition, std::string>::error(node_result.error());
        }
        pipeline.nodes.push_back(std::move(node_result).value());
    }

    // Create linear connections
    for (size_t i = 0; i + 1 < pipeline.nodes.size(); ++i) {
        PipelineConnection conn;
        conn.from_node = pipeline.nodes[i].instance_name;
        conn.to_node = pipeline.nodes[i + 1].instance_name;
        pipeline.connections.push_back(conn);
    }

    // Validate
    auto validate_result = validate(pipeline);
    if (!validate_result.has_value()) {
        return Result<PipelineDefinition, std::string>::error(validate_result.error());
    }

    return Result<PipelineDefinition, std::string>::ok(std::move(pipeline));
}

Result<PipelineDefinition, std::string> PipelineParser::parse_json(
    const std::string& name, const nlohmann::json& j) const {
    PipelineDefinition pipeline;
    pipeline.name = name;

    if (!j.contains("nodes") || !j["nodes"].is_array()) {
        return Result<PipelineDefinition, std::string>::error(
            "Pipeline JSON must contain 'nodes' array");
    }

    int index = 0;
    for (const auto& node_json : j["nodes"]) {
        PipelineNode node;
        node.node_type = node_json.value("type", "");
        node.instance_name = node_json.value("name", node.node_type + "-" + std::to_string(index));
        node.config = node_json.value("config", nlohmann::json::object());

        if (node.node_type.empty()) {
            return Result<PipelineDefinition, std::string>::error(
                "Node at index " + std::to_string(index) + " has no 'type'");
        }

        if (!registry_.has(node.node_type)) {
            return Result<PipelineDefinition, std::string>::error(
                "Unknown node type: " + node.node_type);
        }

        pipeline.nodes.push_back(std::move(node));
        ++index;
    }

    // Parse connections
    if (j.contains("connections") && j["connections"].is_array()) {
        for (const auto& conn_json : j["connections"]) {
            PipelineConnection conn;
            conn.from_node = conn_json.value("from", "");
            conn.to_node = conn_json.value("to", "");

            if (conn.from_node.empty() || conn.to_node.empty()) {
                return Result<PipelineDefinition, std::string>::error(
                    "Connection missing 'from' or 'to'");
            }

            pipeline.connections.push_back(conn);
        }
    } else {
        // Default: linear pipeline
        for (size_t i = 0; i + 1 < pipeline.nodes.size(); ++i) {
            PipelineConnection conn;
            conn.from_node = pipeline.nodes[i].instance_name;
            conn.to_node = pipeline.nodes[i + 1].instance_name;
            pipeline.connections.push_back(conn);
        }
    }

    auto validate_result = validate(pipeline);
    if (!validate_result.has_value()) {
        return Result<PipelineDefinition, std::string>::error(validate_result.error());
    }

    return Result<PipelineDefinition, std::string>::ok(std::move(pipeline));
}

Result<void, std::string> PipelineParser::validate(const PipelineDefinition& pipeline) const {
    if (pipeline.nodes.empty()) {
        return Result<void, std::string>::error("Pipeline has no nodes");
    }

    // Check all node names are unique
    std::set<std::string> names;
    for (const auto& node : pipeline.nodes) {
        if (!names.insert(node.instance_name).second) {
            return Result<void, std::string>::error(
                "Duplicate node name: " + node.instance_name);
        }
    }

    // Check all connections reference valid nodes
    for (const auto& conn : pipeline.connections) {
        if (names.find(conn.from_node) == names.end()) {
            return Result<void, std::string>::error(
                "Connection references unknown node: " + conn.from_node);
        }
        if (names.find(conn.to_node) == names.end()) {
            return Result<void, std::string>::error(
                "Connection references unknown node: " + conn.to_node);
        }
    }

    // Check type compatibility for each connection
    for (const auto& conn : pipeline.connections) {
        // Find the node types
        std::string from_type, to_type;
        std::string from_output, to_input;

        for (const auto& node : pipeline.nodes) {
            if (node.instance_name == conn.from_node) {
                from_type = node.node_type;
                auto nt = registry_.get(from_type);
                if (nt.has_value()) from_output = nt.value()->output_type;
            }
            if (node.instance_name == conn.to_node) {
                to_type = node.node_type;
                auto nt = registry_.get(to_type);
                if (nt.has_value()) to_input = nt.value()->input_type;
            }
        }

        if (!registry_.types_compatible(from_output, to_input)) {
            return Result<void, std::string>::error(
                "Type mismatch: " + conn.from_node + " outputs '" + from_output +
                "' but " + conn.to_node + " expects '" + to_input + "'");
        }
    }

    return Result<void, std::string>::ok();
}

} // namespace straylight
