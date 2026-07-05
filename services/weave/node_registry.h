// services/weave/node_registry.h
// Registry of available pipeline node types for dynamic service composition.
#pragma once

#include <straylight/result.h>

#include <nlohmann/json.hpp>

#include <map>
#include <string>
#include <vector>

namespace straylight {

/// Definition of a pipeline node type.
struct NodeType {
    std::string name;
    std::string description;
    std::string input_type;    // MIME-like type: "video/raw", "audio/pcm", "data/json", "any"
    std::string output_type;
    nlohmann::json config_schema;  // JSON Schema for node configuration
    std::string launch_command;     // Command template to start the node process
    bool is_builtin;               // True for compile-time registered nodes
};

/// Registry of available pipeline node types — built-in + plugin-provided.
class NodeRegistry {
public:
    NodeRegistry();

    /// Register all built-in StrayLight node types.
    void register_builtins();

    /// Scan /etc/straylight/weave.d/ for plugin node definitions.
    Result<int, std::string> scan_plugins();

    /// Register a node type.
    Result<void, std::string> register_node(NodeType node);

    /// Unregister a node type by name.
    Result<void, std::string> unregister_node(const std::string& name);

    /// Get a node type by name.
    Result<const NodeType*, std::string> get(const std::string& name) const;

    /// Check if a node type exists.
    bool has(const std::string& name) const;

    /// List all registered node types.
    std::vector<const NodeType*> list() const;

    /// Check if two node types can be connected (output of A matches input of B).
    bool types_compatible(const std::string& output_type,
                          const std::string& input_type) const;

private:
    std::map<std::string, NodeType> nodes_;

    static constexpr const char* PLUGIN_DIR = "/etc/straylight/weave.d";
};

} // namespace straylight
