// bin/compiler/ir/lowering.cpp
#include "ir/lowering.h"

#include <algorithm>
#include <sstream>

namespace straylight::compiler {

Result<Backend, std::string> backend_from_string(const std::string& s) {
    std::string lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (lower == "cpu")  return Result<Backend, std::string>::ok(Backend::CPU);
    if (lower == "cuda") return Result<Backend, std::string>::ok(Backend::CUDA);
    if (lower == "rocm") return Result<Backend, std::string>::ok(Backend::ROCm);
    return Result<Backend, std::string>::error("unknown backend: " + s);
}

const char* backend_to_string(Backend b) {
    switch (b) {
        case Backend::CPU:  return "cpu";
        case Backend::CUDA: return "cuda";
        case Backend::ROCm: return "rocm";
    }
    return "unknown";
}

std::string Lowerer::format_shape(const std::vector<int64_t>& shape) const {
    std::ostringstream ss;
    ss << "[";
    for (size_t i = 0; i < shape.size(); i++) {
        if (i > 0) ss << ",";
        ss << shape[i];
    }
    ss << "]";
    return ss.str();
}

// ---------------------------------------------------------------------------
// CPU lowering
// ---------------------------------------------------------------------------

std::string Lowerer::lower_node_cpu(const Node& node) const {
    std::string shape_str = format_shape(node.output_desc.shape);
    std::string dtype = node.output_desc.dtype;
    std::string op_name = op_type_to_string(node.op);

    std::ostringstream ss;

    switch (node.op) {
        case OpType::MatMul:
            ss << "cpu_gemm(shape=" << shape_str << ", dtype=" << dtype << ")";
            break;
        case OpType::Conv2d:
            ss << "cpu_conv2d_im2col(shape=" << shape_str << ", dtype=" << dtype << ")";
            break;
        case OpType::ReLU:
            ss << "cpu_relu_vectorized(shape=" << shape_str << ", dtype=" << dtype << ")";
            break;
        case OpType::Add:
            ss << "cpu_elementwise_add(shape=" << shape_str << ", dtype=" << dtype << ")";
            break;
        case OpType::Softmax:
            ss << "cpu_softmax_stable(shape=" << shape_str << ", dtype=" << dtype << ")";
            break;
        case OpType::LayerNorm:
            ss << "cpu_layernorm(shape=" << shape_str << ", dtype=" << dtype << ")";
            break;
        case OpType::Gather:
            ss << "cpu_gather(shape=" << shape_str << ", dtype=" << dtype << ")";
            break;
        case OpType::Reshape:
            ss << "cpu_reshape(shape=" << shape_str << ", dtype=" << dtype << ")";
            break;
        case OpType::Transpose:
            ss << "cpu_transpose(shape=" << shape_str << ", dtype=" << dtype << ")";
            break;
        case OpType::Custom: {
            auto it = node.attrs.find("fused_op");
            std::string custom_name = (it != node.attrs.end()) ? it->second : "custom";
            ss << "cpu_" << custom_name << "(shape=" << shape_str << ", dtype=" << dtype << ")";
            break;
        }
    }

    return ss.str();
}

// ---------------------------------------------------------------------------
// CUDA lowering
// ---------------------------------------------------------------------------

std::string Lowerer::lower_node_cuda(const Node& node) const {
    std::string shape_str = format_shape(node.output_desc.shape);
    std::string dtype = node.output_desc.dtype;

    std::ostringstream ss;

    switch (node.op) {
        case OpType::MatMul:
            ss << "cuda_cublas_gemm(shape=" << shape_str << ", dtype=" << dtype << ")";
            break;
        case OpType::Conv2d:
            ss << "cuda_cudnn_conv2d(shape=" << shape_str << ", dtype=" << dtype << ")";
            break;
        case OpType::ReLU:
            ss << "cuda_relu_kernel<<<grid,block>>>(shape=" << shape_str << ", dtype=" << dtype << ")";
            break;
        case OpType::Add:
            ss << "cuda_add_kernel<<<grid,block>>>(shape=" << shape_str << ", dtype=" << dtype << ")";
            break;
        case OpType::Softmax:
            ss << "cuda_softmax_kernel<<<grid,block>>>(shape=" << shape_str << ", dtype=" << dtype << ")";
            break;
        case OpType::LayerNorm:
            ss << "cuda_layernorm_kernel<<<grid,block>>>(shape=" << shape_str << ", dtype=" << dtype << ")";
            break;
        case OpType::Gather:
            ss << "cuda_gather_kernel<<<grid,block>>>(shape=" << shape_str << ", dtype=" << dtype << ")";
            break;
        case OpType::Reshape:
            ss << "cuda_reshape_view(shape=" << shape_str << ", dtype=" << dtype << ")";
            break;
        case OpType::Transpose:
            ss << "cuda_transpose_kernel<<<grid,block>>>(shape=" << shape_str << ", dtype=" << dtype << ")";
            break;
        case OpType::Custom: {
            auto it = node.attrs.find("fused_op");
            std::string custom_name = (it != node.attrs.end()) ? it->second : "custom";
            ss << "cuda_" << custom_name << "_kernel<<<grid,block>>>(shape=" << shape_str << ", dtype=" << dtype << ")";
            break;
        }
    }

    return ss.str();
}

// ---------------------------------------------------------------------------
// ROCm lowering
// ---------------------------------------------------------------------------

std::string Lowerer::lower_node_rocm(const Node& node) const {
    std::string shape_str = format_shape(node.output_desc.shape);
    std::string dtype = node.output_desc.dtype;

    std::ostringstream ss;

    switch (node.op) {
        case OpType::MatMul:
            ss << "rocm_rocblas_gemm(shape=" << shape_str << ", dtype=" << dtype << ")";
            break;
        case OpType::Conv2d:
            ss << "rocm_miopen_conv2d(shape=" << shape_str << ", dtype=" << dtype << ")";
            break;
        case OpType::ReLU:
            ss << "rocm_relu_hip_kernel(shape=" << shape_str << ", dtype=" << dtype << ")";
            break;
        case OpType::Add:
            ss << "rocm_add_hip_kernel(shape=" << shape_str << ", dtype=" << dtype << ")";
            break;
        case OpType::Softmax:
            ss << "rocm_softmax_hip_kernel(shape=" << shape_str << ", dtype=" << dtype << ")";
            break;
        case OpType::LayerNorm:
            ss << "rocm_layernorm_hip_kernel(shape=" << shape_str << ", dtype=" << dtype << ")";
            break;
        case OpType::Gather:
            ss << "rocm_gather_hip_kernel(shape=" << shape_str << ", dtype=" << dtype << ")";
            break;
        case OpType::Reshape:
            ss << "rocm_reshape_view(shape=" << shape_str << ", dtype=" << dtype << ")";
            break;
        case OpType::Transpose:
            ss << "rocm_transpose_hip_kernel(shape=" << shape_str << ", dtype=" << dtype << ")";
            break;
        case OpType::Custom: {
            auto it = node.attrs.find("fused_op");
            std::string custom_name = (it != node.attrs.end()) ? it->second : "custom";
            ss << "rocm_" << custom_name << "_hip_kernel(shape=" << shape_str << ", dtype=" << dtype << ")";
            break;
        }
    }

    return ss.str();
}

// ---------------------------------------------------------------------------
// Main lower entry point
// ---------------------------------------------------------------------------

Result<std::string, std::string> Lowerer::lower(const Graph& g, Backend backend) {
    auto order = g.topological_order();

    if (order.size() != g.node_count()) {
        return Result<std::string, std::string>::error(
            "graph has cycles; cannot lower");
    }

    std::ostringstream ir;
    ir << "; StrayLight Compiler — lowered IR for " << backend_to_string(backend) << "\n";
    ir << "; nodes: " << g.node_count() << "\n\n";

    for (auto id : order) {
        auto res = g.get_node(id);
        if (!res.has_value()) {
            return Result<std::string, std::string>::error(
                "node " + std::to_string(id) + " missing during lowering");
        }
        const Node* node = res.value();

        // Emit input references.
        ir << "%" << id << " = ";

        std::string lowered;
        switch (backend) {
            case Backend::CPU:  lowered = lower_node_cpu(*node);  break;
            case Backend::CUDA: lowered = lower_node_cuda(*node); break;
            case Backend::ROCm: lowered = lower_node_rocm(*node); break;
        }

        ir << lowered;

        // Annotate inputs.
        if (!node->inputs.empty()) {
            ir << "  ; inputs: ";
            for (size_t i = 0; i < node->inputs.size(); i++) {
                if (i > 0) ir << ", ";
                ir << "%" << node->inputs[i];
            }
        }

        ir << "\n";
    }

    return Result<std::string, std::string>::ok(ir.str());
}

} // namespace straylight::compiler
