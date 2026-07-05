#include <gtest/gtest.h>
#include <straylight/hw/gpu.h>
#include <cstring>

using namespace straylight::hw;

TEST(GpuAllocatorTest, AllocateAndFree) {
    GpuAllocator alloc(GpuBackend::CPU);
    auto result = alloc.allocate(1024);
    ASSERT_TRUE(result.has_value()) << result.error();
    auto ptr = result.value();
    ASSERT_NE(ptr, nullptr);

    std::memset(ptr, 0xAB, 1024);

    alloc.free(ptr);
}

TEST(GpuAllocatorTest, StatsTracking) {
    GpuAllocator alloc(GpuBackend::CPU);
    auto p1 = alloc.allocate(512).value();
    auto p2 = alloc.allocate(256).value();

    auto stats = alloc.stats();
    EXPECT_EQ(stats.allocations, 2u);
    EXPECT_EQ(stats.bytes_allocated, 768u);

    alloc.free(p1);
    alloc.free(p2);

    stats = alloc.stats();
    EXPECT_EQ(stats.bytes_allocated, 0u);
}
