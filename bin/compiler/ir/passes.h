// bin/compiler/ir/passes.h
#pragma once

#include "ir/graph.h"

#include <functional>
#include <string>
#include <vector>

namespace straylight::compiler {

/// Manages a sequence of optimization passes over a computation graph.
class PassManager {
public:
    /// Register a named optimization pass.
    void add_pass(std::string name,
                  std::function<Result<bool, std::string>(Graph&)> pass);

    /// Run all registered passes in order.
    /// Returns total number of passes that made modifications, or an error.
    Result<size_t, std::string> run_all(Graph& g);

private:
    struct PassEntry {
        std::string name;
        std::function<Result<bool, std::string>(Graph&)> fn;
    };
    std::vector<PassEntry> passes_;
};

// ---------------------------------------------------------------------------
// Built-in optimization passes
// ---------------------------------------------------------------------------

/// Fuse MatMul immediately followed by ReLU into a single Custom
/// "FusedMatMulReLU" node, eliminating the separate ReLU.
Result<bool, std::string> fuse_matmul_relu(Graph& g);

/// Remove nodes whose outputs are not consumed by any other node,
/// excluding graph output nodes (nodes with no consumers that are terminal).
/// Iterates until no more dead nodes are found.
Result<bool, std::string> eliminate_dead_nodes(Graph& g);

/// Fold constant Reshape and Transpose nodes that have no dynamic inputs
/// (i.e., their only input is another Reshape or Transpose, forming a chain
/// that can be collapsed into a single operation).
Result<bool, std::string> constant_fold(Graph& g);

} // namespace straylight::compiler
