// tests/unit/subsystems/test_pmem.cpp
// Persistent memory subsystem tests: allocator, WAL.

#include <gtest/gtest.h>

#include "allocator.h"
#include "log.h"
#include "checkpoint.h"

#include <cstring>
#include <filesystem>
#include <vector>

using namespace straylight::pmem;

// ---------------------------------------------------------------------------
// PmemAllocator tests
// ---------------------------------------------------------------------------

TEST(PmemAllocator, InitAndAllocFree) {
    // Use a heap-allocated buffer as simulated pmem.
    std::vector<uint8_t> region(64 * 1024, 0);

    PmemAllocator alloc;
    auto init = alloc.init(region.data(), region.size());
    ASSERT_TRUE(init.has_value());
    EXPECT_EQ(alloc.used(), 0u);

    // Allocate a block.
    auto ptr_result = alloc.alloc(256);
    ASSERT_TRUE(ptr_result.has_value());
    void* ptr = ptr_result.value();
    EXPECT_NE(ptr, nullptr);
    EXPECT_GT(alloc.used(), 0u);

    // Write some data.
    std::memset(ptr, 0xAB, 256);

    // Free.
    auto free_result = alloc.free(ptr);
    EXPECT_TRUE(free_result.has_value());
}

TEST(PmemAllocator, MultipleAllocations) {
    std::vector<uint8_t> region(128 * 1024, 0);

    PmemAllocator alloc;
    ASSERT_TRUE(alloc.init(region.data(), region.size()).has_value());

    std::vector<void*> ptrs;
    for (int i = 0; i < 10; ++i) {
        auto r = alloc.alloc(512);
        ASSERT_TRUE(r.has_value()) << "Allocation " << i << " failed: " << r.error();
        ptrs.push_back(r.value());
    }

    // Free all.
    for (auto p : ptrs) {
        EXPECT_TRUE(alloc.free(p).has_value());
    }
}

TEST(PmemAllocator, NullBaseReturnsError) {
    PmemAllocator alloc;
    auto r = alloc.init(nullptr, 4096);
    EXPECT_FALSE(r.has_value());
}

TEST(PmemAllocator, ZeroBytesAllocReturnsError) {
    std::vector<uint8_t> region(4096, 0);
    PmemAllocator alloc;
    ASSERT_TRUE(alloc.init(region.data(), region.size()).has_value());
    auto r = alloc.alloc(0);
    EXPECT_FALSE(r.has_value());
}

// ---------------------------------------------------------------------------
// WriteAheadLog tests
// ---------------------------------------------------------------------------

class WALTest : public ::testing::Test {
protected:
    std::string wal_path;
    void SetUp() override {
        wal_path = "/tmp/test_wal_" + std::to_string(::getpid()) + ".wal";
        // Clean up from previous runs.
        std::filesystem::remove(wal_path);
    }
    void TearDown() override {
        std::filesystem::remove(wal_path);
    }
};

TEST_F(WALTest, AppendAndRecover) {
    {
        WriteAheadLog wal;
        auto init = wal.init(wal_path, 1024 * 1024);
        ASSERT_TRUE(init.has_value()) << init.error();

        // Append two entries.
        std::string data1 = "hello world";
        auto lsn1 = wal.append(data1.data(), data1.size());
        ASSERT_TRUE(lsn1.has_value()) << lsn1.error();

        std::string data2 = "second entry";
        auto lsn2 = wal.append(data2.data(), data2.size());
        ASSERT_TRUE(lsn2.has_value()) << lsn2.error();

        // Commit only the first.
        ASSERT_TRUE(wal.commit(lsn1.value()).has_value());
    }

    // Re-open and recover.
    {
        WriteAheadLog wal;
        auto init = wal.init(wal_path, 1024 * 1024);
        ASSERT_TRUE(init.has_value()) << init.error();

        auto entries = wal.recover();
        ASSERT_TRUE(entries.has_value()) << entries.error();
        ASSERT_GE(entries.value().size(), 2u);

        // First entry should be committed.
        EXPECT_TRUE(entries.value()[0].committed);
        EXPECT_EQ(std::string(entries.value()[0].data.begin(), entries.value()[0].data.end()),
                  "hello world");

        // Second entry should NOT be committed.
        EXPECT_FALSE(entries.value()[1].committed);
        EXPECT_EQ(std::string(entries.value()[1].data.begin(), entries.value()[1].data.end()),
                  "second entry");
    }
}

TEST_F(WALTest, CommitNonexistentLsnFails) {
    WriteAheadLog wal;
    ASSERT_TRUE(wal.init(wal_path, 1024 * 1024).has_value());
    auto r = wal.commit(999);
    EXPECT_FALSE(r.has_value());
}

// ---------------------------------------------------------------------------
// CheckpointManager tests
// ---------------------------------------------------------------------------

class CheckpointTest : public ::testing::Test {
protected:
    std::string ckpt_dir;
    void SetUp() override {
        ckpt_dir = "/tmp/test_checkpoints_" + std::to_string(::getpid());
        std::filesystem::remove_all(ckpt_dir);
    }
    void TearDown() override {
        std::filesystem::remove_all(ckpt_dir);
    }
};

TEST_F(CheckpointTest, SaveLoadRoundTrip) {
    CheckpointManager mgr(ckpt_dir);

    std::vector<uint8_t> data = {1, 2, 3, 4, 5, 6, 7, 8};
    auto save = mgr.save("test_tensor", data.data(), data.size());
    ASSERT_TRUE(save.has_value()) << save.error();

    auto loaded = mgr.load("test_tensor");
    ASSERT_TRUE(loaded.has_value()) << loaded.error();
    EXPECT_EQ(loaded.value(), data);
}

TEST_F(CheckpointTest, ListAndRemove) {
    CheckpointManager mgr(ckpt_dir);

    std::vector<uint8_t> data = {0xAB};
    ASSERT_TRUE(mgr.save("a", data.data(), data.size()).has_value());
    ASSERT_TRUE(mgr.save("b", data.data(), data.size()).has_value());

    auto list = mgr.list();
    ASSERT_TRUE(list.has_value());
    EXPECT_EQ(list.value().size(), 2u);

    ASSERT_TRUE(mgr.remove("a").has_value());
    list = mgr.list();
    ASSERT_TRUE(list.has_value());
    EXPECT_EQ(list.value().size(), 1u);
}

TEST_F(CheckpointTest, LoadNonexistentFails) {
    CheckpointManager mgr(ckpt_dir);
    auto r = mgr.load("does_not_exist");
    EXPECT_FALSE(r.has_value());
}
