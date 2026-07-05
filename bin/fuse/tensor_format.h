// bin/fuse/tensor_format.h
// Custom .slt tensor file format: header + compressed data
#pragma once

#include "compression.h"

#include <straylight/result.h>

#include <cstdint>
#include <string>
#include <vector>

namespace straylight::fuse_fs {

/// Data types for tensor elements.
enum class TensorDtype : uint8_t {
    Float32 = 0,
    Float16 = 1,
    Int8    = 2,
    Int32   = 3,
    Float64 = 4,
};

/// Tensor metadata stored in the .slt file header.
struct TensorMeta {
    std::string name;                // Tensor name (max 64 chars).
    TensorDtype dtype;               // Element data type.
    std::vector<uint32_t> shape;     // Dimension sizes.
    CompressionType compression;     // Compression method used.
    uint64_t original_size;          // Uncompressed data size in bytes.
    uint64_t compressed_size;        // Compressed data size in bytes.
    uint64_t data_offset;            // Offset of compressed data in file.
};

class TensorFormat {
public:
    /// Parse a .slt header from raw bytes. Returns metadata.
    Result<TensorMeta, std::string> parse_header(const std::vector<uint8_t>& data);

    /// Write a .slt header for the given metadata. Returns header bytes.
    Result<std::vector<uint8_t>, std::string> write_header(const TensorMeta& meta);

    /// Compute the size of a tensor in bytes given dtype and shape.
    static size_t tensor_byte_size(TensorDtype dtype, const std::vector<uint32_t>& shape);

    /// Get element size in bytes for a dtype.
    static size_t dtype_size(TensorDtype dtype);

private:
    // .slt file format:
    // [8 bytes: magic "SLTENSOR"]
    // [4 bytes: version u32]
    // [1 byte: dtype]
    // [1 byte: compression]
    // [1 byte: ndim (number of dimensions)]
    // [1 byte: name_len]
    // [name_len bytes: name]
    // [ndim * 4 bytes: shape (u32 each)]
    // [8 bytes: original_size]
    // [8 bytes: compressed_size]
    // [... compressed data follows ...]

    static constexpr uint64_t SLT_MAGIC = 0x524F534E45544C53ULL; // "SLTENSOR" LE
    static constexpr uint32_t SLT_VERSION = 1;
    static constexpr size_t MIN_HEADER_SIZE = 8 + 4 + 4 + 16; // magic + version + fields + sizes
};

} // namespace straylight::fuse_fs
