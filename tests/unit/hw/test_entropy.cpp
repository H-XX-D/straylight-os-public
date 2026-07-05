#include <gtest/gtest.h>
#include <straylight/hw/entropy.h>

#include <set>

using namespace straylight::hw;

TEST(EntropyTest, GenerateRandomBytes) {
    EntropySource src;
    uint8_t buf[32] = {};
    auto result = src.fill(buf, sizeof(buf));
    ASSERT_TRUE(result.has_value()) << result.error();

    bool all_zero = true;
    for (auto b : buf) { if (b != 0) { all_zero = false; break; } }
    EXPECT_FALSE(all_zero);
}

TEST(EntropyTest, GenerateDistinctValues) {
    EntropySource src;
    std::set<uint64_t> values;
    for (int i = 0; i < 100; i++) {
        uint64_t v = 0;
        src.fill(&v, sizeof(v));
        values.insert(v);
    }
    EXPECT_GE(values.size(), 90u);
}

TEST(EntropyTest, HealthCheckPasses) {
    EntropySource src;
    EXPECT_TRUE(src.health_check().has_value());
}
