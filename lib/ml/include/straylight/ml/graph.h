#pragma once

#include <straylight/export.h>
#include <straylight/types.h>

#include <cstdint>
#include <string>
#include <vector>

namespace straylight::ml {

using NodeId = uint32_t;

struct GraphNode {
    NodeId id;
    std::string name;
    std::string op_type;  // "Input", "MatMul", "ReLU", etc.
    std::vector<NodeId> inputs;
    std::vector<int64_t> output_shape;
};

/// Directed acyclic graph representing a computation.
class STRAYLIGHT_EXPORT Graph {
public:
    explicit Graph(std::string name);

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] size_t num_nodes() const noexcept { return nodes_.size(); }

    /// Add an input node.
    NodeId add_input(std::string name, std::vector<int64_t> shape);

    /// Add an operation node with input dependencies.
    NodeId add_op(std::string op_type, std::vector<NodeId> inputs, std::string name = "");

    /// Get a node by ID.
    [[nodiscard]] const GraphNode& node(NodeId id) const;

    /// Return nodes in topological order.
    [[nodiscard]] std::vector<NodeId> topological_order() const;

private:
    std::string name_;
    std::vector<GraphNode> nodes_;
};

} // namespace straylight::ml
