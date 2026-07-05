// lib/common/include/straylight/types.h
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace straylight {

/// Data types for tensor elements.
enum class DType : uint8_t {
    Float16 = 0,
    Float32 = 1,
    Float64 = 2,
    Int8 = 3,
    Int16 = 4,
    Int32 = 5,
    Int64 = 6,
    UInt8 = 7,
    BFloat16 = 8,
    Float8E4M3 = 9,
    Float8E5M2 = 10,
};

/// Size in bytes of a single element of the given dtype.
constexpr size_t dtype_size(DType dt) {
    switch (dt) {
        case DType::Float8E4M3:
        case DType::Float8E5M2:
        case DType::Int8:
        case DType::UInt8:     return 1;
        case DType::Float16:
        case DType::BFloat16:
        case DType::Int16:     return 2;
        case DType::Float32:
        case DType::Int32:     return 4;
        case DType::Float64:
        case DType::Int64:     return 8;
    }
    return 0;
}

/// Compute device types.
enum class DeviceType : uint8_t {
    CPU = 0,
    CUDA = 1,
    ROCm = 2,
    OneAPI = 3,
    Metal = 4,  // Not supported at runtime, kept for format compat
};

/// Describes a tensor's metadata (shape, dtype, device) without owning data.
struct TensorDesc {
    std::vector<int64_t> shape;
    DType dtype = DType::Float32;
    DeviceType device = DeviceType::CPU;
    int device_id = 0;

    /// Total number of elements.
    [[nodiscard]] int64_t numel() const {
        int64_t n = 1;
        for (auto s : shape) n *= s;
        return n;
    }

    /// Total size in bytes.
    [[nodiscard]] size_t nbytes() const {
        return static_cast<size_t>(numel()) * dtype_size(dtype);
    }
};

/// Subsystem health status.
enum class HealthStatus : uint8_t {
    Healthy = 0,
    Degraded = 1,
    Failed = 2,
    Unknown = 3,
};

} // namespace straylight
