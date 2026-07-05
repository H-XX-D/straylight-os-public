// tests/unit/subsystems/test_rhem.cpp
#include <gtest/gtest.h>

#include "allocator.h"
#include "discovery.h"
#include "migration.h"
#include "policy.h"

using namespace straylight::rhem;

// ── Helper: create a test device set ────────────────────────────────────────

static std::vector<Device> make_test_devices() {
    return {
        {0, DeviceType::CPU,  "Test CPU",  16ULL * 1024 * 1024 * 1024, 0.5f, true},
        {1, DeviceType::CUDA, "Test GPU 0", 8ULL * 1024 * 1024 * 1024, 10.0f, true},
        {2, DeviceType::CUDA, "Test GPU 1", 8ULL * 1024 * 1024 * 1024, 10.0f, true},
        {3, DeviceType::FPGA, "Test FPGA",  4ULL * 1024 * 1024 * 1024, 1.0f, true},
    };
}

// ── Discovery tests ─────────────────────────────────────────────────────────

TEST(Discovery, ScanReturnsAtLeastCPU) {
    DeviceDiscovery disco;
    auto result = disco.scan();
    ASSERT_TRUE(result.has_value());

    const auto& devices = result.value();
    EXPECT_GE(devices.size(), 1u);

    // First device should be CPU
    EXPECT_EQ(devices[0].type, DeviceType::CPU);
    EXPECT_EQ(devices[0].id, 0u);
    EXPECT_TRUE(devices[0].available);
    EXPECT_GT(devices[0].memory_bytes, 0u);
}

TEST(Discovery, DeviceTypeStringRoundTrip) {
    EXPECT_EQ(device_type_str(DeviceType::CPU), "CPU");
    EXPECT_EQ(device_type_str(DeviceType::CUDA), "CUDA");
    EXPECT_EQ(device_type_str(DeviceType::ROCm), "ROCm");
    EXPECT_EQ(device_type_str(DeviceType::FPGA), "FPGA");
    EXPECT_EQ(device_type_str(DeviceType::TPU), "TPU");

    EXPECT_EQ(device_type_from_str("cuda"), DeviceType::CUDA);
    EXPECT_EQ(device_type_from_str("CPU"), DeviceType::CPU);
}

// ── Allocator tests ─────────────────────────────────────────────────────────

TEST(Allocator, BasicAllocateRelease) {
    auto devices = make_test_devices();
    ResourceAllocator alloc(devices);

    auto result = alloc.allocate(DeviceType::CUDA, 1024 * 1024, 0.5f);
    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(result.value().device_id, 1u);  // First CUDA device
    EXPECT_EQ(result.value().memory_bytes, 1024u * 1024u);
    EXPECT_EQ(result.value().compute_fraction, 0.5f);
    EXPECT_EQ(result.value().lease_id, 1u);

    EXPECT_EQ(alloc.active().size(), 1u);

    auto rel = alloc.release(result.value().lease_id);
    ASSERT_TRUE(rel.has_value());
    EXPECT_EQ(alloc.active().size(), 0u);
}

TEST(Allocator, CapacityEnforcement) {
    auto devices = make_test_devices();
    ResourceAllocator alloc(devices);

    // Allocate all compute on GPU 0
    auto r1 = alloc.allocate(DeviceType::CUDA, 1024, 1.0f);
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r1.value().device_id, 1u);

    // Next allocation should go to GPU 1 (GPU 0 is full on compute)
    auto r2 = alloc.allocate(DeviceType::CUDA, 1024, 0.5f);
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2.value().device_id, 2u);

    // Allocate all compute on GPU 1 too
    auto r3 = alloc.allocate(DeviceType::CUDA, 1024, 0.5f);
    ASSERT_TRUE(r3.has_value());
    EXPECT_EQ(r3.value().device_id, 2u);

    // Now both GPUs are full — should fail
    auto r4 = alloc.allocate(DeviceType::CUDA, 1024, 0.1f);
    EXPECT_FALSE(r4.has_value());
}

TEST(Allocator, MemoryCapacity) {
    auto devices = make_test_devices();
    ResourceAllocator alloc(devices);

    // Request more memory than any single GPU has
    auto result = alloc.allocate(DeviceType::CUDA, 100ULL * 1024 * 1024 * 1024, 0.1f);
    EXPECT_FALSE(result.has_value());
}

TEST(Allocator, ReleaseNonexistentLease) {
    auto devices = make_test_devices();
    ResourceAllocator alloc(devices);

    auto result = alloc.release(999);
    EXPECT_FALSE(result.has_value());
}

TEST(Allocator, InvalidComputeFraction) {
    auto devices = make_test_devices();
    ResourceAllocator alloc(devices);

    auto result = alloc.allocate(DeviceType::CPU, 1024, 1.5f);
    EXPECT_FALSE(result.has_value());
}

// ── Migration tests ─────────────────────────────────────────────────────────

TEST(Migrator, PlanValidMigration) {
    auto devices = make_test_devices();
    Migrator mig(devices);

    auto result = mig.plan(0, 1, 1024 * 1024);  // CPU -> GPU, 1MB
    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(result.value().src_device, 0u);
    EXPECT_EQ(result.value().dst_device, 1u);
    EXPECT_EQ(result.value().tensor_bytes, 1024u * 1024u);
    EXPECT_GT(result.value().estimated_time_ms, 0.0f);
}

TEST(Migrator, SameDeviceError) {
    auto devices = make_test_devices();
    Migrator mig(devices);

    auto result = mig.plan(1, 1, 1024);
    EXPECT_FALSE(result.has_value());
}

TEST(Migrator, NonexistentDeviceError) {
    auto devices = make_test_devices();
    Migrator mig(devices);

    auto result = mig.plan(0, 99, 1024);
    EXPECT_FALSE(result.has_value());
}

TEST(Migrator, ExecuteValidPlan) {
    auto devices = make_test_devices();
    Migrator mig(devices);

    auto plan_result = mig.plan(0, 1, 1024 * 1024);
    ASSERT_TRUE(plan_result.has_value());

    auto exec_result = mig.execute(plan_result.value());
    EXPECT_TRUE(exec_result.has_value());
}

TEST(Migrator, ExecuteZeroBytesError) {
    auto devices = make_test_devices();
    Migrator mig(devices);

    MigrationPlan plan;
    plan.src_device = 0;
    plan.dst_device = 1;
    plan.tensor_bytes = 0;
    plan.estimated_time_ms = 0.0f;

    auto result = mig.execute(plan);
    EXPECT_FALSE(result.has_value());
}

// ── Policy tests ────────────────────────────────────────────────────────────

TEST(Policy, RoundRobin) {
    auto devices = make_test_devices();
    PlacementPolicy policy;

    auto r1 = policy.select_device(devices, 1024, PolicyType::RoundRobin);
    auto r2 = policy.select_device(devices, 1024, PolicyType::RoundRobin);
    auto r3 = policy.select_device(devices, 1024, PolicyType::RoundRobin);
    auto r4 = policy.select_device(devices, 1024, PolicyType::RoundRobin);

    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    ASSERT_TRUE(r3.has_value());
    ASSERT_TRUE(r4.has_value());

    // Should cycle through devices
    EXPECT_EQ(r1.value(), devices[0].id);
    EXPECT_EQ(r2.value(), devices[1].id);
    EXPECT_EQ(r3.value(), devices[2].id);
    EXPECT_EQ(r4.value(), devices[3].id);
}

TEST(Policy, LeastLoadedSelectsLargestMemory) {
    auto devices = make_test_devices();
    PlacementPolicy policy;

    auto result = policy.select_device(devices, 1024, PolicyType::LeastLoaded);
    ASSERT_TRUE(result.has_value());

    // CPU has 16GB, GPUs have 8GB each — CPU should be selected
    EXPECT_EQ(result.value(), 0u);
}

TEST(Policy, AffinityPrefersGPU) {
    auto devices = make_test_devices();
    PlacementPolicy policy;

    auto result = policy.select_device(devices, 1024, PolicyType::AffinityBased);
    ASSERT_TRUE(result.has_value());

    // Should prefer CUDA over CPU
    uint32_t selected = result.value();
    EXPECT_TRUE(selected == 1u || selected == 2u)
        << "Affinity policy should prefer CUDA GPU";
}

TEST(Policy, PowerEfficientSelection) {
    auto devices = make_test_devices();
    PlacementPolicy policy;

    auto result = policy.select_device(devices, 1024, PolicyType::PowerEfficient);
    ASSERT_TRUE(result.has_value());
    // Should pick based on TFLOPS/watt ratio
    // CUDA: 10/300 = 0.033, FPGA: 1/75 = 0.013, CPU: 0.5/150 = 0.003
    // CUDA wins
    uint32_t selected = result.value();
    EXPECT_TRUE(selected == 1u || selected == 2u);
}

TEST(Policy, NoEligibleDevice) {
    auto devices = make_test_devices();
    PlacementPolicy policy;

    // Request more memory than any device has
    auto result = policy.select_device(devices, 100ULL * 1024 * 1024 * 1024,
                                       PolicyType::RoundRobin);
    EXPECT_FALSE(result.has_value());
}

TEST(Policy, EmptyDeviceList) {
    PlacementPolicy policy;
    auto result = policy.select_device({}, 1024, PolicyType::RoundRobin);
    EXPECT_FALSE(result.has_value());
}
