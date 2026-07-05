// bin/fuse/compression.h
// Tensor-aware compression: zstd + delta + quantize-compress
#pragma once

#include <straylight/result.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace straylight::fuse_fs {

enum class CompressionType : uint8_t {
    None       = 0, // No compression.
    Zstd       = 1, // Standard zstd compression.
    Delta      = 2, // Delta encoding for sequential data.
    Quantize   = 3, // Quantize float32 -> int8 then compress.
    DeltaZstd  = 4, // Delta encode then zstd.
};

class TensorCompressor {
public:
    /// Compress data using the specified strategy.
    Result<std::vector<uint8_t>, std::string>
    compress(const void* data, size_t size, CompressionType type);

    /// Decompress data. The compression type is stored in the header.
    Result<std::vector<uint8_t>, std::string>
    decompress(const std::vector<uint8_t>& compressed);

private:
    // Compressed block header.
    struct BlockHeader {
        uint32_t magic;
        uint8_t  type;           // CompressionType.
        uint8_t  reserved[3];
        uint64_t original_size;
        uint64_t compressed_size;
    };

    static constexpr uint32_t COMPRESS_MAGIC = 0x534C5443; // "SLTC"
    static constexpr size_t HEADER_SIZE = sizeof(BlockHeader);

    // Compression implementations.
    static std::vector<uint8_t> zstd_compress(const uint8_t* data, size_t len);
    static std::vector<uint8_t> zstd_decompress(const uint8_t* data, size_t len, size_t orig_size);
    static std::vector<uint8_t> delta_encode(const uint8_t* data, size_t len);
    static std::vector<uint8_t> delta_decode(const uint8_t* data, size_t len);
    static std::vector<uint8_t> quantize_compress(const uint8_t* data, size_t len);
    static std::vector<uint8_t> quantize_decompress(const uint8_t* data, size_t len, size_t orig_size);
};

} // namespace straylight::fuse_fs
