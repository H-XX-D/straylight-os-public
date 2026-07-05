#pragma once

#include <straylight/export.h>
#include <straylight/result.h>
#include <straylight/ml/tensor.h>

#include <functional>
#include <string>

namespace straylight::ml {

/// Framework interception bridge for PyTorch, JAX, TensorFlow, ONNX.
/// Implementation deferred to Plan 5 (ML Subsystems).
/// This header defines the interface that framework bridge modules will implement.
enum class Framework : uint8_t {
    PyTorch = 0,
    JAX = 1,
    TensorFlow = 2,
    ONNX = 3,
};

/// Callback signature for intercepting framework allocations.
using AllocInterceptFn = std::function<void*(size_t bytes, int device_id)>;
using FreeInterceptFn = std::function<void(void* ptr)>;

/// Register an allocation interceptor for a specific framework.
/// When a framework calls malloc/cudaMalloc, StrayLight's LIR layer
/// routes it through this interceptor to use VPU slab allocation.
STRAYLIGHT_EXPORT
Result<void, std::string> register_alloc_interceptor(
    Framework fw, AllocInterceptFn alloc_fn, FreeInterceptFn free_fn);

/// Unregister a previously registered interceptor.
STRAYLIGHT_EXPORT
Result<void, std::string> unregister_alloc_interceptor(Framework fw);

} // namespace straylight::ml
