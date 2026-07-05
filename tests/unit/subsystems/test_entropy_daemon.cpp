#include <gtest/gtest.h>
#include "drbg.h"

#include <algorithm>

using namespace straylight;

TEST(Drbg, GeneratesNonZeroBytes) {
    CtrDrbg drbg;
    std::array<uint8_t, 32> seed{};
    std::fill(seed.begin(), seed.end(), 0xAB);
    ASSERT_TRUE(drbg.seed(seed).has_value());

    auto out = drbg.generate(32);
    ASSERT_TRUE(out.has_value());
    bool all_zero = std::all_of(out.value().begin(), out.value().end(),
                                [](uint8_t b) { return b == 0; });
    EXPECT_FALSE(all_zero);
}

TEST(Drbg, DifferentSeedsDifferentOutput) {
    CtrDrbg drbg1, drbg2;
    std::array<uint8_t, 32> seed1{}, seed2{};
    seed1.fill(0x11);
    seed2.fill(0x22);
    ASSERT_TRUE(drbg1.seed(seed1).has_value());
    ASSERT_TRUE(drbg2.seed(seed2).has_value());

    auto out1 = drbg1.generate(32);
    auto out2 = drbg2.generate(32);
    ASSERT_TRUE(out1.has_value());
    ASSERT_TRUE(out2.has_value());
    EXPECT_NE(out1.value(), out2.value());
}

TEST(Drbg, ReseedChangesOutput) {
    CtrDrbg drbg;
    std::array<uint8_t, 32> seed{};
    seed.fill(0x55);
    ASSERT_TRUE(drbg.seed(seed).has_value());
    auto out1 = drbg.generate(16);

    seed.fill(0x66);
    ASSERT_TRUE(drbg.reseed(seed).has_value());
    auto out2 = drbg.generate(16);

    ASSERT_TRUE(out1.has_value());
    ASSERT_TRUE(out2.has_value());
    EXPECT_NE(out1.value(), out2.value());
}

TEST(Drbg, UnseededGenerateFails) {
    CtrDrbg drbg;
    auto out = drbg.generate(32);
    EXPECT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), SLErrorCode::NotInitialized);
}

TEST(Drbg, UnseededReseedFails) {
    CtrDrbg drbg;
    std::array<uint8_t, 32> data{};
    auto res = drbg.reseed(data);
    EXPECT_FALSE(res.has_value());
    EXPECT_EQ(res.error().code(), SLErrorCode::NotInitialized);
}

TEST(Drbg, DeterministicWithSameSeed) {
    // Two DRBGs seeded identically should produce identical output.
    CtrDrbg drbg1, drbg2;
    std::array<uint8_t, 32> seed{};
    seed.fill(0xCC);
    ASSERT_TRUE(drbg1.seed(seed).has_value());
    ASSERT_TRUE(drbg2.seed(seed).has_value());

    auto out1 = drbg1.generate(64);
    auto out2 = drbg2.generate(64);
    ASSERT_TRUE(out1.has_value());
    ASSERT_TRUE(out2.has_value());
    EXPECT_EQ(out1.value(), out2.value());
}

TEST(Drbg, DoesNotUseLegacyXorCounterBlock) {
    CtrDrbg drbg;
    std::array<uint8_t, 32> seed{};
    seed.fill(0xAB);
    ASSERT_TRUE(drbg.seed(seed).has_value());

    auto out = drbg.generate(16);
    ASSERT_TRUE(out.has_value());

    // The retired placeholder produced key[i] ^ counter[i] ^ key[i+16],
    // which collapses this seed to mostly zeros with a counter byte.
    std::vector<uint8_t> legacy_xor_block(16, 0);
    legacy_xor_block[15] = 0x02;
    EXPECT_NE(out.value(), legacy_xor_block);
}

TEST(Drbg, StatsTrackGenerationAndReseed) {
    CtrDrbg drbg;
    std::array<uint8_t, 32> seed{};
    seed.fill(0x3C);
    ASSERT_TRUE(drbg.seed(seed).has_value());

    auto out = drbg.generate(24);
    ASSERT_TRUE(out.has_value());
    seed.fill(0xA5);
    ASSERT_TRUE(drbg.reseed(seed).has_value());

    auto stats = drbg.stats();
    EXPECT_TRUE(stats.seeded);
    EXPECT_TRUE(stats.health_ok);
    EXPECT_EQ(stats.algorithm, "ctr-drbg-aes-256");
    EXPECT_EQ(stats.bytes_generated, 24u);
    EXPECT_EQ(stats.generate_calls, 1u);
    EXPECT_EQ(stats.reseed_count, 1u);
    EXPECT_TRUE(stats.last_error.empty());
}
