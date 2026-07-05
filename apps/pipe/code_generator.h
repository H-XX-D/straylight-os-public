// apps/pipe/code_generator.h
// StrayLight Pipe — C++ code generation from a validated pipeline.
#pragma once

#include "pipeline.h"

#include <straylight/result.h>

#include <string>
#include <vector>

namespace straylight::pipe {

/// Generates a standalone, compilable C++ source file from a Pipeline.
///
/// The emitted code:
/// - Includes the correct headers based on which node types appear.
/// - Allocates intermediate buffers between nodes.
/// - Launches parallel branches via a thread pool.
/// - Uses Result<T,E> for error handling.
class CodeGenerator {
public:
    explicit CodeGenerator(const Pipeline& pipeline);

    /// Generate the full source string.
    Result<std::string, std::string> generate() const;

private:
    /// Emit #include lines for the node types present.
    std::string emit_includes(const std::vector<uint32_t>& order) const;

    /// Emit buffer declarations.
    std::string emit_buffers(const std::vector<uint32_t>& order) const;

    /// Emit the processing body in topological order.
    std::string emit_body(const std::vector<uint32_t>& order) const;

    /// Emit the function call for one node.
    std::string emit_node_call(const PipeNode& node, int buf_index) const;

    /// Detect independent branches that can run in parallel.
    std::vector<std::vector<uint32_t>> find_parallel_groups(
        const std::vector<uint32_t>& order) const;

    const Pipeline& pipeline_;
};

} // namespace straylight::pipe
