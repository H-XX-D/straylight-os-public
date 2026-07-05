// bin/compiler/ir/graph.cpp
#include "ir/graph.h"

#include <algorithm>
#include <queue>
#include <sstream>

#include <nlohmann/json.hpp>

namespace straylight::compiler {

// ---------------------------------------------------------------------------
// OpType string conversion
// ---------------------------------------------------------------------------

static const std::pair<OpType, const char*> op_names[] = {
    {OpType::MatMul,    "MatMul"},
    {OpType::Conv2d,    "Conv2d"},
    {OpType::ReLU,      "ReLU"},
    {OpType::Add,       "Add"},
    {OpType::Softmax,   "Softmax"},
    {OpType::LayerNorm, "LayerNorm"},
    {OpType::Gather,    "Gather"},
    {OpType::Reshape,   "Reshape"},
    {OpType::Transpose, "Transpose"},
    {OpType::Custom,    "Custom"},
};

const char* op_type_to_string(OpType op) {
    for (const auto& [o, name] : op_names) {
        if (o == op) return name;
    }
    return "Unknown";
}

Result<OpType, std::string> op_type_from_string(const std::string& s) {
    for (const auto& [o, name] : op_names) {
        if (s == name) return Result<OpType, std::string>::ok(o);
    }
    return Result<OpType, std::string>::error("unknown OpType: " + s);
}

// ---------------------------------------------------------------------------
// Graph implementation
// ---------------------------------------------------------------------------

Result<uint64_t, std::string> Graph::add_node(
    OpType op,
    std::vector<uint64_t> inputs,
    TensorDesc out,
    std::unordered_map<std::string, std::string> attrs)
{
    // Validate all input IDs exist (empty inputs is valid for source nodes).
    for (auto in_id : inputs) {
        if (nodes_.find(in_id) == nodes_.end()) {
            return Result<uint64_t, std::string>::error(
                "input node " + std::to_string(in_id) + " does not exist");
        }
    }

    uint64_t id = next_id_++;
    Node node{};
    node.id = id;
    node.op = op;
    node.inputs = std::move(inputs);
    node.outputs = {};
    node.output_desc = std::move(out);
    node.attrs = std::move(attrs);

    // Register this node as an output consumer of each input.
    for (auto in_id : node.inputs) {
        nodes_[in_id].outputs.push_back(id);
    }

    nodes_.emplace(id, std::move(node));
    return Result<uint64_t, std::string>::ok(id);
}

Result<const Node*, std::string> Graph::get_node(uint64_t id) const {
    auto it = nodes_.find(id);
    if (it == nodes_.end()) {
        return Result<const Node*, std::string>::error(
            "node " + std::to_string(id) + " not found");
    }
    return Result<const Node*, std::string>::ok(&it->second);
}

Result<void, std::string> Graph::remove_node(uint64_t id) {
    auto it = nodes_.find(id);
    if (it == nodes_.end()) {
        return Result<void, std::string>::error(
            "node " + std::to_string(id) + " not found");
    }

    const Node& node = it->second;

    // Remove this node from the outputs list of each of its inputs.
    for (auto in_id : node.inputs) {
        auto in_it = nodes_.find(in_id);
        if (in_it != nodes_.end()) {
            auto& outs = in_it->second.outputs;
            outs.erase(std::remove(outs.begin(), outs.end(), id), outs.end());
        }
    }

    // Remove this node from the inputs list of each of its consumers.
    for (auto out_id : node.outputs) {
        auto out_it = nodes_.find(out_id);
        if (out_it != nodes_.end()) {
            auto& ins = out_it->second.inputs;
            ins.erase(std::remove(ins.begin(), ins.end(), id), ins.end());
        }
    }

    nodes_.erase(it);
    return Result<void, std::string>::ok();
}

Result<void, std::string> Graph::replace_node_op(
    uint64_t id, OpType new_op,
    std::unordered_map<std::string, std::string> new_attrs)
{
    auto it = nodes_.find(id);
    if (it == nodes_.end()) {
        return Result<void, std::string>::error(
            "node " + std::to_string(id) + " not found");
    }
    it->second.op = new_op;
    if (!new_attrs.empty()) {
        it->second.attrs = std::move(new_attrs);
    }
    return Result<void, std::string>::ok();
}

std::vector<uint64_t> Graph::topological_order() const {
    // Kahn's algorithm.
    // Build in-degree map from our node set.
    std::unordered_map<uint64_t, size_t> in_degree;
    for (const auto& [id, node] : nodes_) {
        // Ensure every node has an entry.
        if (in_degree.find(id) == in_degree.end()) {
            in_degree[id] = 0;
        }
        // Count only inputs that still exist in the graph.
        for (auto in_id : node.inputs) {
            if (nodes_.find(in_id) != nodes_.end()) {
                in_degree[id]++;
            }
        }
    }

    std::queue<uint64_t> ready;
    for (const auto& [id, deg] : in_degree) {
        if (deg == 0) {
            ready.push(id);
        }
    }

    std::vector<uint64_t> order;
    order.reserve(nodes_.size());

    while (!ready.empty()) {
        uint64_t cur = ready.front();
        ready.pop();
        order.push_back(cur);

        auto it = nodes_.find(cur);
        if (it == nodes_.end()) continue;

        for (auto out_id : it->second.outputs) {
            if (in_degree.find(out_id) == in_degree.end()) continue;
            in_degree[out_id]--;
            if (in_degree[out_id] == 0) {
                ready.push(out_id);
            }
        }
    }

    return order;
}

size_t Graph::node_count() const {
    return nodes_.size();
}

std::vector<uint64_t> Graph::output_nodes() const {
    std::vector<uint64_t> result;
    for (const auto& [id, node] : nodes_) {
        if (node.outputs.empty()) {
            result.push_back(id);
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// JSON serialization
// ---------------------------------------------------------------------------

Result<std::string, std::string> Graph::serialize_json() const {
    try {
        nlohmann::json j;
        j["next_id"] = next_id_;

        nlohmann::json nodes_arr = nlohmann::json::array();
        // Serialize in topological order for determinism.
        auto order = topological_order();
        for (auto id : order) {
            auto it = nodes_.find(id);
            if (it == nodes_.end()) continue;
            const Node& n = it->second;

            nlohmann::json nj;
            nj["id"] = n.id;
            nj["op"] = op_type_to_string(n.op);
            nj["inputs"] = n.inputs;
            nj["outputs"] = n.outputs;
            nj["output_desc"]["shape"] = n.output_desc.shape;
            nj["output_desc"]["dtype"] = n.output_desc.dtype;
            nj["attrs"] = n.attrs;
            nodes_arr.push_back(std::move(nj));
        }
        j["nodes"] = std::move(nodes_arr);

        return Result<std::string, std::string>::ok(j.dump(2));
    } catch (const std::exception& e) {
        return Result<std::string, std::string>::error(
            std::string("serialize failed: ") + e.what());
    }
}

Result<Graph, std::string> Graph::deserialize_json(const std::string& json_str) {
    try {
        auto j = nlohmann::json::parse(json_str);

        Graph g;
        g.next_id_ = j.at("next_id").get<uint64_t>();

        // First pass: insert all nodes without validating inputs,
        // because forward references are possible in a serialized graph.
        for (const auto& nj : j.at("nodes")) {
            Node n{};
            n.id = nj.at("id").get<uint64_t>();

            auto op_result = op_type_from_string(nj.at("op").get<std::string>());
            if (!op_result.has_value()) {
                return Result<Graph, std::string>::error(op_result.error());
            }
            n.op = op_result.value();

            n.inputs = nj.at("inputs").get<std::vector<uint64_t>>();
            n.outputs = nj.at("outputs").get<std::vector<uint64_t>>();

            n.output_desc.shape =
                nj.at("output_desc").at("shape").get<std::vector<int64_t>>();
            n.output_desc.dtype =
                nj.at("output_desc").at("dtype").get<std::string>();

            if (nj.contains("attrs")) {
                n.attrs = nj.at("attrs")
                    .get<std::unordered_map<std::string, std::string>>();
            }

            g.nodes_.emplace(n.id, std::move(n));
        }

        // Validate referential integrity.
        for (const auto& [id, node] : g.nodes_) {
            for (auto in_id : node.inputs) {
                if (g.nodes_.find(in_id) == g.nodes_.end()) {
                    return Result<Graph, std::string>::error(
                        "node " + std::to_string(id) +
                        " references nonexistent input " + std::to_string(in_id));
                }
            }
            for (auto out_id : node.outputs) {
                if (g.nodes_.find(out_id) == g.nodes_.end()) {
                    return Result<Graph, std::string>::error(
                        "node " + std::to_string(id) +
                        " references nonexistent output " + std::to_string(out_id));
                }
            }
        }

        return Result<Graph, std::string>::ok(std::move(g));
    } catch (const nlohmann::json::exception& e) {
        return Result<Graph, std::string>::error(
            std::string("JSON parse error: ") + e.what());
    } catch (const std::exception& e) {
        return Result<Graph, std::string>::error(
            std::string("deserialize failed: ") + e.what());
    }
}

} // namespace straylight::compiler
