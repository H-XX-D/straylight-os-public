#include <gtest/gtest.h>
#include <straylight/ml/kv_cache.h>

using namespace straylight::ml;

TEST(KvCacheTest, PutAndGet) {
    KvCache cache(3);
    cache.put("key1", Tensor({1, 4}, straylight::DType::Float32));
    auto* t = cache.get("key1");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->numel(), 4);
}

TEST(KvCacheTest, MissReturnsNull) {
    KvCache cache(3);
    EXPECT_EQ(cache.get("nonexistent"), nullptr);
}

TEST(KvCacheTest, EvictsLRU) {
    KvCache cache(2);
    cache.put("a", Tensor({1}, straylight::DType::Float32));
    cache.put("b", Tensor({2}, straylight::DType::Float32));
    cache.get("a");  // touch "a" so "b" is LRU
    cache.put("c", Tensor({3}, straylight::DType::Float32));  // should evict "b"

    EXPECT_NE(cache.get("a"), nullptr);
    EXPECT_EQ(cache.get("b"), nullptr);  // evicted
    EXPECT_NE(cache.get("c"), nullptr);
}

TEST(KvCacheTest, SizeTracking) {
    KvCache cache(10);
    EXPECT_EQ(cache.size(), 0u);
    cache.put("x", Tensor({4}, straylight::DType::Float32));
    EXPECT_EQ(cache.size(), 1u);
}
