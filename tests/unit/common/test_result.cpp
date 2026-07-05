// tests/unit/common/test_result.cpp
#include <gtest/gtest.h>
#include <straylight/result.h>
#include <string>

using namespace straylight;

TEST(ResultTest, OkValueCanBeAccessed) {
    auto r = Result<int, std::string>::ok(42);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r.value(), 42);
}

TEST(ResultTest, ErrorCanBeAccessed) {
    auto r = Result<int, std::string>::error("something failed");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), "something failed");
}

TEST(ResultTest, MapTransformsValue) {
    auto r = Result<int, std::string>::ok(10);
    auto mapped = r.map([](int v) { return v * 2; });
    ASSERT_TRUE(mapped.has_value());
    EXPECT_EQ(mapped.value(), 20);
}

TEST(ResultTest, MapPassesThroughError) {
    auto r = Result<int, std::string>::error("fail");
    auto mapped = r.map([](int v) { return v * 2; });
    ASSERT_FALSE(mapped.has_value());
    EXPECT_EQ(mapped.error(), "fail");
}

TEST(ResultTest, AndThenChainsOperations) {
    auto r = Result<int, std::string>::ok(10);
    auto chained = r.and_then([](int v) -> Result<std::string, std::string> {
        return Result<std::string, std::string>::ok(std::to_string(v));
    });
    ASSERT_TRUE(chained.has_value());
    EXPECT_EQ(chained.value(), "10");
}

TEST(ResultTest, ValueOrReturnsDefaultOnError) {
    auto r = Result<int, std::string>::error("fail");
    EXPECT_EQ(r.value_or(99), 99);
}
