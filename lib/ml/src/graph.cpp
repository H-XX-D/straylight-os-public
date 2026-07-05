#include <straylight/ml/graph.h>

#include <algorithm>
#include <queue>
#include <stdexcept>
#include <unordered_map>

namespace straylight::ml {

Graph::Graph(std::string name) : name_(std::move(name)) {}

NodeId Graph::add_input(std::string name, std::vector<int64_t> shape) {
    NodeId id = static_cast<NodeId>(nodes_.size());
    nodes_.push_back({id, std::move(name), "Input", {}, std::move(shape)});
    return id;
}

NodeId Graph::add_op(std::string op_type, std::vector<NodeId> inputs, std::string name) {
    for (auto inp : inputs) {
        if (inp >= static_cast<NodeId>(nodes_.size())) {
            throw std::out_of_range("Invalid input node ID: " + std::to_string(inp));
        }
    }
    NodeId id = static_cast<NodeId>(nodes_.size());
    if (name.empty()) {
        name = op_type + "_" + std::to_string(id);
    }
    nodes_.push_back({id, std::move(name), std::move(op_type), std::move(inputs), {}});
    return id;
}

const GraphNode& Graph::node(NodeId id) const {
    if (id >= nodes_.size()) {
        throw std::out_of_range("Invalid node ID");
    }
    return nodes_[id];
}

std::vector<NodeId> Graph::topological_order() const {
    // Kahn's algorithm
    std::unordered_map<NodeId, int> in_degree;
    for (auto& n : nodes_) {
        if (in_degree.find(n.id) == in_degree.end()) in_degree[n.id] = 0;
        in_degree[n.id] += static_cast<int>(n.inputs.size());
    }

    std::queue<NodeId> q;
    for (auto& [id, deg] : in_degree) {
        if (deg == 0) q.push(id);
    }

    std::vector<NodeId> order;
    while (!q.empty()) {
        auto cur = q.front();
        q.pop();
        order.push_back(cur);

        for (auto& n : nodes_) {
            for (auto dep : n.inputs) {
                if (dep == cur) {
                    if (--in_degree[n.id] == 0) {
                        q.push(n.id);
                    }
                }
            }
        }
    }

    if (order.size() != nodes_.size()) {
        throw std::runtime_error("Graph contains a cycle (" +
            std::to_string(order.size()) + " of " +
            std::to_string(nodes_.size()) + " nodes reachable)");
    }

    return order;
}

} // namespace straylight::ml
