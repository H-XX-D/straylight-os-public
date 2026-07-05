// bin/compiler/ir/graph.h
#pragma once

#include <straylight/result.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace straylight::compiler {

enum class OpType : uint8_t {
    MatMul,
    Conv2d,
    ReLU,
    Add,
    Softmax,
    LayerNorm,
    Gather,
    Reshape,
    Transpose,
    Custom
};

/// Convert OpType to its string name.
const char* op_type_to_string(OpType op);

/// Parse an OpType from its string name.
Result<OpType, std::string> op_type_from_string(const std::string& s);

/// Description of a tensor's shape and element type.
struct TensorDesc {
    std::vector<int64_t> shape;
    std::string dtype; // "f32", "f16", "i8"
};

/// A single computation node in the graph IR.
struct Node {
    uint64_t id;
    OpType op;
    std::vector<uint64_t> inputs;   // node IDs that feed into this node
    std::vector<uint64_t> outputs;  // node IDs that consume this node's output
    TensorDesc output_desc;
    std::unordered_map<std::string, std::string> attrs;
};

/// Computation graph intermediate representation.
/// Nodes are connected by ID references. The graph supports serialization
/// to/from JSON and topological ordering via Kahn's algorithm.
class Graph {
public:
    /// Add a node with the given operation, inputs, output description, and attributes.
    /// Returns the assigned node ID, or an error if any input ID is invalid.
    Result<uint64_t, std::string> add_node(
        OpType op,
        std::vector<uint64_t> inputs,
        TensorDesc out,
        std::unordered_map<std::string, std::string> attrs = {});

    /// Look up a node by ID.
    Result<const Node*, std::string> get_node(uint64_t id) const;

    /// Remove a node by ID and clean up all references to it.
    Result<void, std::string> remove_node(uint64_t id);

    /// Replace a node's op and attributes in-place, keeping its connections.
    Result<void, std::string> replace_node_op(
        uint64_t id, OpType new_op,
        std::unordered_map<std::string, std::string> new_attrs = {});

    /// Return node IDs in topological order (Kahn's algorithm).
    std::vector<uint64_t> topological_order() const;

    /// Number of nodes currently in the graph.
    size_t node_count() const;

    /// Serialize the entire graph to a JSON string.
    Result<std::string, std::string> serialize_json() const;

    /// Deserialize a graph from a JSON string.
    static Result<Graph, std::string> deserialize_json(const std::string& json);

    /// Direct access to the node map (for passes that need iteration).
    const std::unordered_map<uint64_t, Node>& nodes() const { return nodes_; }

    /// Get all terminal (output) node IDs: nodes whose outputs list is empty.
    std::vector<uint64_t> output_nodes() const;

private:
    uint64_t next_id_ = 1;
    std::unordered_map<uint64_t, Node> nodes_;
};

} // namespace straylight::compiler
