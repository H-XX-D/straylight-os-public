// tests/unit/subsystems/test_fuse.cpp
// FUSE subsystem tests: compressor round-trip, tensor format, block cache.

#include <gtest/gtest.h>

#include "compression.h"
#include "tensor_format.h"
#include "cache.h"

#include <cmath>
#include <cstring>
#include <numeric>
#include <vector>

using namespace straylight::fuse_fs;

// ---------------------------------------------------------------------------
// TensorCompressor round-trip tests
// ---------------------------------------------------------------------------

TEST(Compressor, NoneRoundTrip) {
    TensorCompressor comp;

    std::vector<uint8_t> data = {1, 2, 3, 4, 5, 6, 7, 8};
    auto compressed = comp.compress(data.data(), data.size(), CompressionType::None);
    ASSERT_TRUE(compressed.has_value()) << compressed.error();

    auto decompressed = comp.decompress(compressed.value());
    ASSERT_TRUE(decompressed.has_value()) << decompressed.error();

    EXPECT_EQ(decompressed.value(), data);
}

TEST(Compressor, DeltaRoundTrip) {
    TensorCompressor comp;

    // Sequential data benefits from delta encoding.
    std::vector<uint8_t> data(256);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>(i & 0xFF);
    }

    auto compressed = comp.compress(data.data(), data.size(), CompressionType::Delta);
    ASSERT_TRUE(compressed.has_value()) << compressed.error();

    auto decompressed = comp.decompress(compressed.value());
    ASSERT_TRUE(decompressed.has_value()) << decompressed.error();

    EXPECT_EQ(decompressed.value(), data);
}

TEST(Compressor, ZstdRoundTrip) {
    TensorCompressor comp;

    // Create data with some repetition (good for LZ compression).
    std::vector<uint8_t> data(4096);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>((i * 7 + 13) % 256);
    }

    auto compressed = comp.compress(data.data(), data.size(), CompressionType::Zstd);
    ASSERT_TRUE(compressed.has_value()) << compressed.error();

    auto decompressed = comp.decompress(compressed.value());
    ASSERT_TRUE(decompressed.has_value()) << decompressed.error();

    EXPECT_EQ(decompressed.value(), data);
}

TEST(Compressor, DeltaZstdRoundTrip) {
    TensorCompressor comp;

    std::vector<uint8_t> data(1024);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>(i % 128);
    }

    auto compressed = comp.compress(data.data(), data.size(), CompressionType::DeltaZstd);
    ASSERT_TRUE(compressed.has_value()) << compressed.error();

    auto decompressed = comp.decompress(compressed.value());
    ASSERT_TRUE(decompressed.has_value()) << decompressed.error();

    EXPECT_EQ(decompressed.value(), data);
}

TEST(Compressor, QuantizeRoundTripApproximate) {
    TensorCompressor comp;

    // Create float data.
    std::vector<float> floats = {0.0f, 0.5f, 1.0f, -1.0f, 3.14f, -2.71f, 100.0f, -100.0f};
    auto* data = reinterpret_cast<const uint8_t*>(floats.data());
    size_t data_size = floats.size() * sizeof(float);

    auto compressed = comp.compress(data, data_size, CompressionType::Quantize);
    ASSERT_TRUE(compressed.has_value()) << compressed.error();

    auto decompressed = comp.decompress(compressed.value());
    ASSERT_TRUE(decompressed.has_value()) << decompressed.error();

    // Quantization is lossy — check approximate equality.
    ASSERT_EQ(decompressed.value().size(), data_size);
    auto* result = reinterpret_cast<const float*>(decompressed.value().data());
    float max_range = 200.0f; // -100 to 100
    float tolerance = max_range / 255.0f * 2.0f; // ~1.57
    for (size_t i = 0; i < floats.size(); ++i) {
        EXPECT_NEAR(result[i], floats[i], tolerance)
            << "Mismatch at index " << i;
    }
}

TEST(Compressor, EmptyDataReturnsError) {
    TensorCompressor comp;
    auto r = comp.compress(nullptr, 0, CompressionType::None);
    EXPECT_FALSE(r.has_value());
}

// ---------------------------------------------------------------------------
// TensorFormat tests
// ---------------------------------------------------------------------------

TEST(TensorFormat, WriteAndParseHeader) {
    TensorFormat fmt;

    TensorMeta meta;
    meta.name = "test_weight";
    meta.dtype = TensorDtype::Float32;
    meta.shape = {32, 64, 3};
    meta.compression = CompressionType::Zstd;
    meta.original_size = 32 * 64 * 3 * 4; // float32
    meta.compressed_size = 12345;

    auto header = fmt.write_header(meta);
    ASSERT_TRUE(header.has_value()) << header.error();

    auto parsed = fmt.parse_header(header.value());
    ASSERT_TRUE(parsed.has_value()) << parsed.error();

    EXPECT_EQ(parsed.value().name, "test_weight");
    EXPECT_EQ(parsed.value().dtype, TensorDtype::Float32);
    EXPECT_EQ(parsed.value().shape, meta.shape);
    EXPECT_EQ(parsed.value().compression, CompressionType::Zstd);
    EXPECT_EQ(parsed.value().original_size, meta.original_size);
    EXPECT_EQ(parsed.value().compressed_size, meta.compressed_size);
}

TEST(TensorFormat, TensorByteSize) {
    EXPECT_EQ(TensorFormat::tensor_byte_size(TensorDtype::Float32, {10, 20}), 800u);
    EXPECT_EQ(TensorFormat::tensor_byte_size(TensorDtype::Int8, {100}), 100u);
    EXPECT_EQ(TensorFormat::tensor_byte_size(TensorDtype::Float64, {3, 4, 5}), 480u);
}

TEST(TensorFormat, DtypeSize) {
    EXPECT_EQ(TensorFormat::dtype_size(TensorDtype::Float32), 4u);
    EXPECT_EQ(TensorFormat::dtype_size(TensorDtype::Float16), 2u);
    EXPECT_EQ(TensorFormat::dtype_size(TensorDtype::Int8), 1u);
    EXPECT_EQ(TensorFormat::dtype_size(TensorDtype::Int32), 4u);
    EXPECT_EQ(TensorFormat::dtype_size(TensorDtype::Float64), 8u);
}

TEST(TensorFormat, InvalidMagicReturnsError) {
    TensorFormat fmt;
    std::vector<uint8_t> garbage(64, 0xFF);
    auto r = fmt.parse_header(garbage);
    EXPECT_FALSE(r.has_value());
}

// ---------------------------------------------------------------------------
// BlockCache tests
// ---------------------------------------------------------------------------

TEST(BlockCache, PutAndGet) {
    BlockCache cache(1024 * 1024); // 1MB

    std::vector<uint8_t> data = {1, 2, 3, 4};
    cache.put("/test.slt", 0, data);

    auto result = cache.get("/test.slt", 0, 4);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), data);
    EXPECT_EQ(cache.hits(), 1u);
}

TEST(BlockCache, MissReturnsError) {
    BlockCache cache(1024 * 1024);
    auto result = cache.get("/nonexistent", 0, 4);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(cache.misses(), 1u);
}

TEST(BlockCache, EvictRemovesEntries) {
    BlockCache cache(1024 * 1024);

    std::vector<uint8_t> data = {1, 2, 3};
    cache.put("/a.slt", 0, data);
    cache.put("/a.slt", 100, data);
    cache.put("/b.slt", 0, data);

    EXPECT_EQ(cache.block_count(), 3u);

    cache.evict("/a.slt");
    EXPECT_EQ(cache.block_count(), 1u);
}

TEST(BlockCache, LRUEvictionOnCapacity) {
    BlockCache cache(100); // Very small cache.

    std::vector<uint8_t> big(60, 0xAB);
    cache.put("/a", 0, big);
    cache.put("/b", 0, big); // This should evict /a.

    auto a = cache.get("/a", 0, 60);
    EXPECT_FALSE(a.has_value()); // /a should have been evicted.

    auto b = cache.get("/b", 0, 60);
    EXPECT_TRUE(b.has_value());
}
