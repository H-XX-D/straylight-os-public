// tests/unit/subsystems/test_fuse_compression.cpp
// Unit tests for the tensor-aware compression/decompression codec.
// Exercises LZ4 (Zstd-compatible wrapper), delta, quantize, and delta+zstd modes.

#include <gtest/gtest.h>

#include "compression.h"

#include <cstdint>
#include <vector>

using namespace straylight::fuse_fs;

/// 256 KiB test block size (matches the on-disk block unit).
static constexpr size_t TEST_BLOCK_SIZE = 256 * 1024;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Generate a block of mostly-zero data with sparse non-zero values.
static std::vector<uint8_t> make_sparse_block(size_t size, size_t nonzero_stride) {
    std::vector<uint8_t> data(size, 0);
    for (size_t i = 0; i < size; i += nonzero_stride) {
        data[i] = 0x42;
    }
    return data;
}

/// Generate a block with a repeating linear pattern.
static std::vector<uint8_t> make_pattern_block(size_t size) {
    std::vector<uint8_t> data(size);
    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<uint8_t>(i & 0xFF);
    }
    return data;
}

// ---------------------------------------------------------------------------
// None (passthrough) codec
// ---------------------------------------------------------------------------

TEST(TensorCompression, NoneCodecPreservesData) {
    TensorCompressor c;
    std::vector<uint8_t> data = {0x11, 0x22, 0x33, 0x44};

    auto compressed = c.compress(data.data(), data.size(), CompressionType::None);
    ASSERT_TRUE(compressed.has_value()) << compressed.error();

    auto decompressed = c.decompress(compressed.value());
    ASSERT_TRUE(decompressed.has_value()) << decompressed.error();
    EXPECT_EQ(decompressed.value(), data);
}

// ---------------------------------------------------------------------------
// Zstd codec (internal LZ77-based compression)
// ---------------------------------------------------------------------------

TEST(TensorCompression, ZstdRoundTripPatternBlock) {
    TensorCompressor c;
    auto data = make_pattern_block(TEST_BLOCK_SIZE);

    auto compressed = c.compress(data.data(), data.size(), CompressionType::Zstd);
    ASSERT_TRUE(compressed.has_value()) << compressed.error();

    auto decompressed = c.decompress(compressed.value());
    ASSERT_TRUE(decompressed.has_value()) << decompressed.error();
    EXPECT_EQ(decompressed.value(), data);
}

TEST(TensorCompression, ZstdCompressesSparseData) {
    TensorCompressor c;
    // A block that is 99% zeros compresses very well.
    auto data = make_sparse_block(TEST_BLOCK_SIZE, 1000);

    auto compressed = c.compress(data.data(), data.size(), CompressionType::Zstd);
    ASSERT_TRUE(compressed.has_value()) << compressed.error();

    // Compressed size should be significantly smaller.
    EXPECT_LT(compressed.value().size(), data.size());

    auto decompressed = c.decompress(compressed.value());
    ASSERT_TRUE(decompressed.has_value()) << decompressed.error();
    EXPECT_EQ(decompressed.value(), data);
}

TEST(TensorCompression, ZstdSmallBlock) {
    TensorCompressor c;
    std::vector<uint8_t> data = {1, 2, 3, 4, 5, 6, 7, 8};

    auto compressed = c.compress(data.data(), data.size(), CompressionType::Zstd);
    ASSERT_TRUE(compressed.has_value()) << compressed.error();

    auto decompressed = c.decompress(compressed.value());
    ASSERT_TRUE(decompressed.has_value()) << decompressed.error();
    EXPECT_EQ(decompressed.value(), data);
}

// ---------------------------------------------------------------------------
// Delta codec
// ---------------------------------------------------------------------------

TEST(TensorCompression, DeltaRoundTripMonotonic) {
    TensorCompressor c;
    // Monotonically increasing data — delta produces all-1s, compresses well.
    std::vector<uint8_t> data(256);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>(i);
    }

    auto compressed = c.compress(data.data(), data.size(), CompressionType::Delta);
    ASSERT_TRUE(compressed.has_value()) << compressed.error();

    auto decompressed = c.decompress(compressed.value());
    ASSERT_TRUE(decompressed.has_value()) << decompressed.error();
    EXPECT_EQ(decompressed.value(), data);
}

TEST(TensorCompression, DeltaRoundTripLargeBlock) {
    TensorCompressor c;
    auto data = make_pattern_block(64 * 1024);

    auto compressed = c.compress(data.data(), data.size(), CompressionType::Delta);
    ASSERT_TRUE(compressed.has_value()) << compressed.error();

    auto decompressed = c.decompress(compressed.value());
    ASSERT_TRUE(decompressed.has_value()) << decompressed.error();
    EXPECT_EQ(decompressed.value(), data);
}

// ---------------------------------------------------------------------------
// DeltaZstd (chain) codec
// ---------------------------------------------------------------------------

TEST(TensorCompression, DeltaZstdRoundTrip) {
    TensorCompressor c;
    auto data = make_pattern_block(TEST_BLOCK_SIZE);

    auto compressed = c.compress(data.data(), data.size(), CompressionType::DeltaZstd);
    ASSERT_TRUE(compressed.has_value()) << compressed.error();

    auto decompressed = c.decompress(compressed.value());
    ASSERT_TRUE(decompressed.has_value()) << decompressed.error();
    EXPECT_EQ(decompressed.value(), data);
}

TEST(TensorCompression, DeltaZstdBetterThanZstdAloneOnMonotonic) {
    TensorCompressor c;
    // Monotonic sequence: DeltaZstd produces all-constant delta, should be
    // at least as small as plain Zstd.
    std::vector<uint8_t> data(TEST_BLOCK_SIZE);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>(i & 0xFF);
    }

    auto cz = c.compress(data.data(), data.size(), CompressionType::Zstd);
    auto cdz = c.compress(data.data(), data.size(), CompressionType::DeltaZstd);
    ASSERT_TRUE(cz.has_value());
    ASSERT_TRUE(cdz.has_value());

    // DeltaZstd should produce equal or smaller output for monotonic sequences.
    EXPECT_LE(cdz.value().size(), cz.value().size() + 100 /* tolerance */);

    // Verify round-trip.
    auto decompressed = c.decompress(cdz.value());
    ASSERT_TRUE(decompressed.has_value());
    EXPECT_EQ(decompressed.value(), data);
}

// ---------------------------------------------------------------------------
// Quantize codec (float32 tensor data)
// ---------------------------------------------------------------------------

TEST(TensorCompression, QuantizeRoundTripApproximate) {
    TensorCompressor c;
    // Build a float32 tensor: 256 floats linearly spaced [0.0, 1.0].
    constexpr size_t N = 256;
    std::vector<float> floats(N);
    for (size_t i = 0; i < N; ++i) {
        floats[i] = static_cast<float>(i) / static_cast<float>(N - 1);
    }

    const auto* bytes = reinterpret_cast<const uint8_t*>(floats.data());
    const size_t byte_sz = N * sizeof(float);

    auto compressed = c.compress(bytes, byte_sz, CompressionType::Quantize);
    ASSERT_TRUE(compressed.has_value()) << compressed.error();
    // Quantized output should be smaller than raw float32 data.
    EXPECT_LT(compressed.value().size(), byte_sz);

    auto decompressed = c.decompress(compressed.value());
    ASSERT_TRUE(decompressed.has_value()) << decompressed.error();
    ASSERT_EQ(decompressed.value().size(), byte_sz);

    // Dequantised floats should be within quantisation error tolerance (~1/255).
    const auto* recovered = reinterpret_cast<const float*>(decompressed.value().data());
    for (size_t i = 0; i < N; ++i) {
        EXPECT_NEAR(recovered[i], floats[i], 0.01f)
            << "Mismatch at element " << i;
    }
}

// ---------------------------------------------------------------------------
// Error path tests
// ---------------------------------------------------------------------------

TEST(TensorCompression, NullDataReturnsError) {
    TensorCompressor c;
    auto r = c.compress(nullptr, 128, CompressionType::Zstd);
    EXPECT_FALSE(r.has_value());
}

TEST(TensorCompression, ZeroSizeReturnsError) {
    TensorCompressor c;
    uint8_t dummy = 0;
    auto r = c.compress(&dummy, 0, CompressionType::Zstd);
    EXPECT_FALSE(r.has_value());
}

TEST(TensorCompression, DecompressEmptyReturnsError) {
    TensorCompressor c;
    std::vector<uint8_t> empty;
    auto r = c.decompress(empty);
    EXPECT_FALSE(r.has_value());
}

TEST(TensorCompression, DecompressBadMagicReturnsError) {
    TensorCompressor c;
    // Deliberately wrong magic + plausible-sized buffer.
    std::vector<uint8_t> bad(64, 0xBB);
    auto r = c.decompress(bad);
    EXPECT_FALSE(r.has_value());
}

// ---------------------------------------------------------------------------
// Idempotency: compress twice and get same result
// ---------------------------------------------------------------------------

TEST(TensorCompression, CompressTwiceGivesSameSize) {
    TensorCompressor c;
    auto data = make_pattern_block(4096);

    auto c1 = c.compress(data.data(), data.size(), CompressionType::Zstd);
    auto c2 = c.compress(data.data(), data.size(), CompressionType::Zstd);
    ASSERT_TRUE(c1.has_value());
    ASSERT_TRUE(c2.has_value());
    EXPECT_EQ(c1.value().size(), c2.value().size());
}
