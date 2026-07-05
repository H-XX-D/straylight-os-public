// bin/compiler/ir/lowering.h
#pragma once

#include "ir/graph.h"

#include <string>

namespace straylight::compiler {

/// Target backend for code lowering.
enum class Backend : uint8_t {
    CPU,
    CUDA,
    ROCm
};

/// Parse a Backend from a string ("cpu", "cuda", "rocm").
Result<Backend, std::string> backend_from_string(const std::string& s);

/// Convert a Backend to its string name.
const char* backend_to_string(Backend b);

/// Lowers a computation graph to backend-specific pseudo-IR.
class Lowerer {
public:
    /// Lower the graph for the given backend.
    /// Returns a pseudo-IR string representing the lowered operations.
    Result<std::string, std::string> lower(const Graph& g, Backend backend);

private:
    std::string lower_node_cpu(const Node& node) const;
    std::string lower_node_cuda(const Node& node) const;
    std::string lower_node_rocm(const Node& node) const;
    std::string format_shape(const std::vector<int64_t>& shape) const;
};

} // namespace straylight::compiler
