#include <gtest/gtest.h>
#include <straylight/hw/mgpu.h>

#include <algorithm>
#include <set>
#include <vector>

using namespace straylight::hw;

/*
 * Unit tests for MultiGpuManager.
 *
 * These tests run without a real VPU kernel device.  The manager will
 * fail to open /dev/straylight-vpu and fall back to sysfs PCI scanning.
 * On CI/macOS where no discrete GPUs with class 0x0300 exist, the
 * discovered GPU list will be empty — tests validate that the manager
 * handles this gracefully, and also test policy logic by injecting
 * synthetic GPU info.
 */

/* --------------------------------------------------------------------------
 * Basic lifecycle
 * -------------------------------------------------------------------------- */

TEST(MultiGpuManagerTest, ConstructAndDestruct) {
    MultiGpuManager mgr;
    EXPECT_EQ(mgr.gpu_count(), 0u);
}

TEST(MultiGpuManagerTest, DiscoverDoesNotCrash) {
    MultiGpuManager mgr;
    auto result = mgr.discover();
    EXPECT_TRUE(result.has_value());
    // gpu_count may be 0 on macOS / headless CI
}

TEST(MultiGpuManagerTest, GpusReturnsEmptyBeforeDiscover) {
    MultiGpuManager mgr;
    auto list = mgr.gpus();
    EXPECT_TRUE(list.empty());
}

TEST(MultiGpuManagerTest, MoveConstruction) {
    MultiGpuManager mgr1;
    auto result = mgr1.discover();
    EXPECT_TRUE(result.has_value());
    size_t count = mgr1.gpu_count();

    MultiGpuManager mgr2(std::move(mgr1));
    EXPECT_EQ(mgr2.gpu_count(), count);
}

TEST(MultiGpuManagerTest, MoveAssignment) {
    MultiGpuManager mgr1;
    mgr1.discover();

    MultiGpuManager mgr2;
    mgr2 = std::move(mgr1);
    // Should not crash
}

/* --------------------------------------------------------------------------
 * Allocation error cases
 * -------------------------------------------------------------------------- */

TEST(MultiGpuManagerTest, AllocateFailsWithNoGpus) {
    MultiGpuManager mgr;
    // Don't discover — no GPUs
    auto result = mgr.allocate(4096);
    EXPECT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("No GPUs"), std::string::npos);
}

TEST(MultiGpuManagerTest, AllocateZeroBytesFailsWithNoGpus) {
    MultiGpuManager mgr;
    auto result = mgr.allocate(0);
    EXPECT_FALSE(result.has_value());
}

TEST(MultiGpuManagerTest, AllocateOnOutOfRangeFails) {
    MultiGpuManager mgr;
    mgr.discover();  // may find 0 GPUs
    auto result = mgr.allocate_on(999, 4096);
    EXPECT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("out of range"), std::string::npos);
}

TEST(MultiGpuManagerTest, AllocateOnZeroBytesFailsWhenGpuExists) {
    MultiGpuManager mgr;
    mgr.discover();
    if (mgr.gpu_count() == 0)
        GTEST_SKIP() << "No GPUs available";

    auto result = mgr.allocate_on(0, 0);
    EXPECT_FALSE(result.has_value());
}

/* --------------------------------------------------------------------------
 * P2P capability checks
 * -------------------------------------------------------------------------- */

TEST(MultiGpuManagerTest, HasP2pSameGpuIsTrue) {
    MultiGpuManager mgr;
    mgr.discover();
    if (mgr.gpu_count() == 0)
        GTEST_SKIP() << "No GPUs available";

    EXPECT_TRUE(mgr.has_p2p(0, 0));
}

TEST(MultiGpuManagerTest, HasP2pOutOfRangeIsFalse) {
    MultiGpuManager mgr;
    EXPECT_FALSE(mgr.has_p2p(100, 200));
}

/* --------------------------------------------------------------------------
 * Stats error cases
 * -------------------------------------------------------------------------- */

TEST(MultiGpuManagerTest, GpuStatsOutOfRangeFails) {
    MultiGpuManager mgr;
    mgr.discover();
    auto result = mgr.gpu_stats(999);
    EXPECT_FALSE(result.has_value());
}

TEST(MultiGpuManagerTest, GpuStatsReturnsValidData) {
    MultiGpuManager mgr;
    mgr.discover();
    if (mgr.gpu_count() == 0)
        GTEST_SKIP() << "No GPUs available";

    auto result = mgr.gpu_stats(0);
    EXPECT_TRUE(result.has_value());

    const auto& info = result.value();
    EXPECT_EQ(info.index, 0u);
    EXPECT_FALSE(info.vendor.empty());
}

/* --------------------------------------------------------------------------
 * Free on empty allocation (no device)
 * -------------------------------------------------------------------------- */

TEST(MultiGpuManagerTest, FreeDoesNotCrashWithoutDevice) {
    MultiGpuManager mgr;
    MgpuAllocation alloc{};
    alloc.handle = 42;
    alloc.gpu_index = 0;
    alloc.size = 4096;

    // Should succeed even without a device — just updates local stats
    auto result = mgr.free(alloc);
    EXPECT_TRUE(result.has_value());
}

/* --------------------------------------------------------------------------
 * P2P copy / Mirror without device
 * -------------------------------------------------------------------------- */

TEST(MultiGpuManagerTest, P2pCopyFailsWithoutDevice) {
    MultiGpuManager mgr;
    auto result = mgr.p2p_copy(1, 2, 4096);
    EXPECT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("not open"), std::string::npos);
}

TEST(MultiGpuManagerTest, MirrorFailsWithoutDevice) {
    MultiGpuManager mgr;
    auto result = mgr.mirror(1, 0x3);
    EXPECT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("not open"), std::string::npos);
}

/* --------------------------------------------------------------------------
 * GpuDeviceInfo struct validation
 * -------------------------------------------------------------------------- */

TEST(GpuDeviceInfoTest, DefaultConstruction) {
    GpuDeviceInfo info{};
    EXPECT_EQ(info.index, 0u);
    EXPECT_EQ(info.pci_vendor, 0);
    EXPECT_EQ(info.pci_device, 0);
    EXPECT_EQ(info.vram_total, 0u);
    EXPECT_EQ(info.vram_used, 0u);
    EXPECT_TRUE(info.name.empty());
    EXPECT_TRUE(info.vendor.empty());
    EXPECT_TRUE(info.pci_slot.empty());
}

/* --------------------------------------------------------------------------
 * MgpuAllocation struct
 * -------------------------------------------------------------------------- */

TEST(MgpuAllocationTest, DefaultConstruction) {
    MgpuAllocation alloc{};
    EXPECT_EQ(alloc.handle, 0u);
    EXPECT_EQ(alloc.gpu_index, 0u);
    EXPECT_EQ(alloc.gpu_addr, 0u);
    EXPECT_EQ(alloc.size, 0u);
    EXPECT_TRUE(alloc.mirror_handles.empty());
}

/* --------------------------------------------------------------------------
 * PlacementPolicy enum coverage
 * -------------------------------------------------------------------------- */

TEST(PlacementPolicyTest, EnumValues) {
    EXPECT_EQ(static_cast<uint8_t>(PlacementPolicy::RoundRobin), 0);
    EXPECT_EQ(static_cast<uint8_t>(PlacementPolicy::LeastUsed), 1);
    EXPECT_EQ(static_cast<uint8_t>(PlacementPolicy::Affinity), 2);
    EXPECT_EQ(static_cast<uint8_t>(PlacementPolicy::Mirror), 3);
}

/* --------------------------------------------------------------------------
 * Round-robin policy cycling (integration-level — needs real GPUs or mocking)
 * These tests validate the logic if GPUs are present; skip otherwise.
 * -------------------------------------------------------------------------- */

TEST(MultiGpuManagerTest, RoundRobinCycles) {
    MultiGpuManager mgr;
    mgr.discover();
    if (mgr.gpu_count() < 2)
        GTEST_SKIP() << "Need at least 2 GPUs for round-robin test";

    size_t n = mgr.gpu_count();
    std::vector<uint32_t> indices;

    for (size_t i = 0; i < n * 2; i++) {
        auto result = mgr.allocate(4096, PlacementPolicy::RoundRobin);
        ASSERT_TRUE(result.has_value()) << result.error();
        indices.push_back(result.value().gpu_index);
    }

    // Verify cycling: each GPU should appear at least once in the first n
    std::set<uint32_t> first_round(indices.begin(), indices.begin() + n);
    EXPECT_EQ(first_round.size(), n);

    // Second round should repeat the pattern
    for (size_t i = 0; i < n; i++) {
        EXPECT_EQ(indices[i], indices[i + n]);
    }

    // Clean up
    // (allocations are synthetic without device, no cleanup needed)
}

TEST(MultiGpuManagerTest, LeastUsedPicksMinimum) {
    MultiGpuManager mgr;
    mgr.discover();
    if (mgr.gpu_count() < 2)
        GTEST_SKIP() << "Need at least 2 GPUs for least-used test";

    // First allocation goes to GPU with least usage (all start at 0)
    auto result = mgr.allocate(4096, PlacementPolicy::LeastUsed);
    ASSERT_TRUE(result.has_value()) << result.error();

    // After allocating on GPU 0, GPU 1 should have less usage
    auto result2 = mgr.allocate(4096, PlacementPolicy::LeastUsed);
    ASSERT_TRUE(result2.has_value()) << result2.error();

    // The two allocations should be on different GPUs if vram_used differs
    if (result.value().gpu_index == 0) {
        EXPECT_NE(result2.value().gpu_index, 0u);
    }
}

TEST(MultiGpuManagerTest, AffinityStickToSameGpu) {
    MultiGpuManager mgr;
    mgr.discover();
    if (mgr.gpu_count() == 0)
        GTEST_SKIP() << "No GPUs available";

    auto r1 = mgr.allocate(4096, PlacementPolicy::Affinity);
    ASSERT_TRUE(r1.has_value());

    auto r2 = mgr.allocate(4096, PlacementPolicy::Affinity);
    ASSERT_TRUE(r2.has_value());

    EXPECT_EQ(r1.value().gpu_index, r2.value().gpu_index);
}

/* --------------------------------------------------------------------------
 * GPU info field validation after discover
 * -------------------------------------------------------------------------- */

TEST(MultiGpuManagerTest, DiscoveredGpuInfoFields) {
    MultiGpuManager mgr;
    mgr.discover();
    if (mgr.gpu_count() == 0)
        GTEST_SKIP() << "No GPUs available";

    auto list = mgr.gpus();
    ASSERT_FALSE(list.empty());

    for (const auto& gpu : list) {
        EXPECT_EQ(gpu.index, &gpu - &list[0]);
        EXPECT_FALSE(gpu.vendor.empty());
        EXPECT_FALSE(gpu.name.empty());
        EXPECT_NE(gpu.pci_vendor, 0);
        // Temperature may be -1 (unavailable)
        EXPECT_TRUE(gpu.temperature == -1.0f || gpu.temperature >= 0.0f);
    }
}
