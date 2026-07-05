// bin/fuse/compression.cpp
#include "compression.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace straylight::fuse_fs {

Result<std::vector<uint8_t>, std::string>
TensorCompressor::compress(const void* data, size_t size, CompressionType type) {
    if (!data || size == 0) {
        return Result<std::vector<uint8_t>, std::string>::error("Cannot compress empty data");
    }

    auto* src = reinterpret_cast<const uint8_t*>(data);
    std::vector<uint8_t> payload;

    switch (type) {
        case CompressionType::None:
            payload.assign(src, src + size);
            break;
        case CompressionType::Zstd:
            payload = zstd_compress(src, size);
            break;
        case CompressionType::Delta:
            payload = delta_encode(src, size);
            break;
        case CompressionType::Quantize:
            payload = quantize_compress(src, size);
            break;
        case CompressionType::DeltaZstd: {
            auto delta = delta_encode(src, size);
            payload = zstd_compress(delta.data(), delta.size());
            break;
        }
    }

    if (payload.empty() && type != CompressionType::None) {
        return Result<std::vector<uint8_t>, std::string>::error("Compression produced empty output");
    }

    // Prepend header.
    std::vector<uint8_t> result(HEADER_SIZE + payload.size());
    BlockHeader hdr{};
    hdr.magic = COMPRESS_MAGIC;
    hdr.type = static_cast<uint8_t>(type);
    hdr.original_size = size;
    hdr.compressed_size = payload.size();
    std::memcpy(result.data(), &hdr, HEADER_SIZE);
    std::memcpy(result.data() + HEADER_SIZE, payload.data(), payload.size());

    return Result<std::vector<uint8_t>, std::string>::ok(std::move(result));
}

Result<std::vector<uint8_t>, std::string>
TensorCompressor::decompress(const std::vector<uint8_t>& compressed) {
    if (compressed.size() < HEADER_SIZE) {
        return Result<std::vector<uint8_t>, std::string>::error("Data too small for header");
    }

    BlockHeader hdr{};
    std::memcpy(&hdr, compressed.data(), HEADER_SIZE);

    if (hdr.magic != COMPRESS_MAGIC) {
        return Result<std::vector<uint8_t>, std::string>::error("Invalid compression magic");
    }
    if (HEADER_SIZE + hdr.compressed_size > compressed.size()) {
        return Result<std::vector<uint8_t>, std::string>::error("Truncated compressed data");
    }

    const uint8_t* payload = compressed.data() + HEADER_SIZE;
    auto type = static_cast<CompressionType>(hdr.type);
    std::vector<uint8_t> result;

    switch (type) {
        case CompressionType::None:
            result.assign(payload, payload + hdr.compressed_size);
            break;
        case CompressionType::Zstd:
            result = zstd_decompress(payload, hdr.compressed_size, hdr.original_size);
            break;
        case CompressionType::Delta:
            result = delta_decode(payload, hdr.compressed_size);
            break;
        case CompressionType::Quantize:
            result = quantize_decompress(payload, hdr.compressed_size, hdr.original_size);
            break;
        case CompressionType::DeltaZstd: {
            auto zstd_decoded = zstd_decompress(payload, hdr.compressed_size, hdr.original_size);
            result = delta_decode(zstd_decoded.data(), zstd_decoded.size());
            break;
        }
    }

    return Result<std::vector<uint8_t>, std::string>::ok(std::move(result));
}

// ============================================================================
// Zstd-compatible LZ compression (simplified implementation)
// Uses a hash-chain matching approach for LZ77-style compression.
// ============================================================================

std::vector<uint8_t> TensorCompressor::zstd_compress(const uint8_t* data, size_t len) {
    // Simple LZ77-style compression with 4-byte hash matching.
    // Output format: [literal_count:varint][literals...][match_offset:u16][match_len:u8]...
    // Sequence: blocks of literals followed by match references.

    if (len <= 16) {
        // Too small to compress meaningfully — store as-is with a literal header.
        std::vector<uint8_t> out;
        out.push_back(0xFF); // Marker: uncompressed.
        out.insert(out.end(), data, data + len);
        return out;
    }

    std::vector<uint8_t> out;
    out.push_back(0x00); // Marker: compressed.

    // Hash table for match finding (position of last seen 4-byte hash).
    constexpr size_t HASH_SIZE = 1 << 14;
    std::vector<int32_t> hash_table(HASH_SIZE, -1);

    auto hash4 = [](const uint8_t* p) -> uint32_t {
        uint32_t v;
        std::memcpy(&v, p, 4);
        return (v * 2654435761u) >> 18;
    };

    size_t pos = 0;
    size_t lit_start = 0;

    while (pos + 4 <= len) {
        uint32_t h = hash4(data + pos);
        int32_t match_pos = hash_table[h];
        hash_table[h] = static_cast<int32_t>(pos);

        if (match_pos >= 0 &&
            pos - static_cast<size_t>(match_pos) < 65536 &&
            std::memcmp(data + pos, data + match_pos, 4) == 0) {
            // Found a match. Emit literals first.
            size_t lit_len = pos - lit_start;

            // Encode literal length as varint.
            size_t l = lit_len;
            while (l >= 128) {
                out.push_back(static_cast<uint8_t>(l & 0x7F) | 0x80);
                l >>= 7;
            }
            out.push_back(static_cast<uint8_t>(l));

            // Emit literal bytes.
            out.insert(out.end(), data + lit_start, data + lit_start + lit_len);

            // Extend match as far as possible.
            size_t match_len = 4;
            while (pos + match_len < len &&
                   data[pos + match_len] == data[match_pos + match_len] &&
                   match_len < 255) {
                ++match_len;
            }

            // Emit match: offset (2 bytes LE) + length (1 byte).
            uint16_t offset = static_cast<uint16_t>(pos - static_cast<size_t>(match_pos));
            out.push_back(static_cast<uint8_t>(offset & 0xFF));
            out.push_back(static_cast<uint8_t>(offset >> 8));
            out.push_back(static_cast<uint8_t>(match_len));

            pos += match_len;
            lit_start = pos;
        } else {
            ++pos;
        }
    }

    // Emit remaining literals.
    size_t lit_len = len - lit_start;
    size_t l = lit_len;
    while (l >= 128) {
        out.push_back(static_cast<uint8_t>(l & 0x7F) | 0x80);
        l >>= 7;
    }
    out.push_back(static_cast<uint8_t>(l));
    out.insert(out.end(), data + lit_start, data + len);

    // Terminal: zero-length match.
    out.push_back(0);
    out.push_back(0);
    out.push_back(0);

    return out;
}

std::vector<uint8_t> TensorCompressor::zstd_decompress(const uint8_t* data, size_t len,
                                                         size_t orig_size) {
    if (len == 0) return {};

    std::vector<uint8_t> out;
    out.reserve(orig_size);

    if (data[0] == 0xFF) {
        // Uncompressed block.
        out.assign(data + 1, data + len);
        return out;
    }

    // Compressed block.
    size_t pos = 1; // Skip marker byte.

    while (pos < len) {
        // Read literal length (varint).
        size_t lit_len = 0;
        size_t shift = 0;
        while (pos < len) {
            uint8_t b = data[pos++];
            lit_len |= static_cast<size_t>(b & 0x7F) << shift;
            shift += 7;
            if ((b & 0x80) == 0) break;
        }

        // Copy literals.
        if (pos + lit_len > len) break;
        out.insert(out.end(), data + pos, data + pos + lit_len);
        pos += lit_len;

        // Read match (offset:u16 + length:u8).
        if (pos + 3 > len) break;
        uint16_t offset = static_cast<uint16_t>(data[pos]) |
                          (static_cast<uint16_t>(data[pos + 1]) << 8);
        uint8_t match_len = data[pos + 2];
        pos += 3;

        if (offset == 0 && match_len == 0) break; // Terminal.

        // Copy match from output history.
        if (offset == 0 || offset > out.size()) break;
        size_t match_start = out.size() - offset;
        for (size_t i = 0; i < match_len; ++i) {
            out.push_back(out[match_start + i]);
        }
    }

    return out;
}

// ============================================================================
// Delta encoding: store differences between consecutive bytes.
// ============================================================================

std::vector<uint8_t> TensorCompressor::delta_encode(const uint8_t* data, size_t len) {
    std::vector<uint8_t> out(len);
    if (len == 0) return out;
    out[0] = data[0];
    for (size_t i = 1; i < len; ++i) {
        out[i] = data[i] - data[i - 1];
    }
    return out;
}

std::vector<uint8_t> TensorCompressor::delta_decode(const uint8_t* data, size_t len) {
    std::vector<uint8_t> out(len);
    if (len == 0) return out;
    out[0] = data[0];
    for (size_t i = 1; i < len; ++i) {
        out[i] = out[i - 1] + data[i];
    }
    return out;
}

// ============================================================================
// Quantize-compress: float32 -> int8 (with scale/offset) then delta-zstd.
// Header: [min:float32][max:float32][count:u32][quantized_bytes...]
// ============================================================================

std::vector<uint8_t> TensorCompressor::quantize_compress(const uint8_t* data, size_t len) {
    size_t num_floats = len / sizeof(float);
    if (num_floats == 0 || len % sizeof(float) != 0) {
        // Not float data — fall back to delta encoding.
        return delta_encode(data, len);
    }

    auto* floats = reinterpret_cast<const float*>(data);

    // Find min/max for quantization range.
    float fmin = std::numeric_limits<float>::max();
    float fmax = std::numeric_limits<float>::lowest();
    for (size_t i = 0; i < num_floats; ++i) {
        if (std::isfinite(floats[i])) {
            fmin = std::min(fmin, floats[i]);
            fmax = std::max(fmax, floats[i]);
        }
    }

    float range = fmax - fmin;
    if (range < 1e-10f) range = 1.0f; // Avoid division by zero.
    float scale = 255.0f / range;

    // Quantize to uint8.
    std::vector<uint8_t> quantized(num_floats);
    for (size_t i = 0; i < num_floats; ++i) {
        float val = std::isfinite(floats[i]) ? floats[i] : 0.0f;
        float normalized = (val - fmin) * scale;
        quantized[i] = static_cast<uint8_t>(
            std::clamp(normalized, 0.0f, 255.0f));
    }

    // Build output: header + quantized data.
    size_t hdr_size = sizeof(float) * 2 + sizeof(uint32_t);
    std::vector<uint8_t> out(hdr_size + num_floats);
    std::memcpy(out.data(), &fmin, sizeof(float));
    std::memcpy(out.data() + sizeof(float), &fmax, sizeof(float));
    uint32_t count = static_cast<uint32_t>(num_floats);
    std::memcpy(out.data() + sizeof(float) * 2, &count, sizeof(uint32_t));
    std::memcpy(out.data() + hdr_size, quantized.data(), num_floats);

    return out;
}

std::vector<uint8_t> TensorCompressor::quantize_decompress(const uint8_t* data, size_t len,
                                                              size_t orig_size) {
    size_t hdr_size = sizeof(float) * 2 + sizeof(uint32_t);
    if (len < hdr_size) {
        // Fall back to delta decode.
        return delta_decode(data, len);
    }

    float fmin, fmax;
    uint32_t count;
    std::memcpy(&fmin, data, sizeof(float));
    std::memcpy(&fmax, data + sizeof(float), sizeof(float));
    std::memcpy(&count, data + sizeof(float) * 2, sizeof(uint32_t));

    if (len < hdr_size + count) {
        return delta_decode(data, len);
    }

    float range = fmax - fmin;
    if (range < 1e-10f) range = 1.0f;
    float inv_scale = range / 255.0f;

    // Dequantize.
    std::vector<uint8_t> out(count * sizeof(float));
    auto* floats = reinterpret_cast<float*>(out.data());
    const uint8_t* quantized = data + hdr_size;

    for (uint32_t i = 0; i < count; ++i) {
        floats[i] = fmin + static_cast<float>(quantized[i]) * inv_scale;
    }

    return out;
}

} // namespace straylight::fuse_fs
