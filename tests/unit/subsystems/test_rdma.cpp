// tests/unit/subsystems/test_rdma.cpp
#include <gtest/gtest.h>
#include "verbs.h"
#include "memory_region.h"
#include "queue_pair.h"
#include "tensor_rdma.h"

using namespace straylight::rdma;

// ─── VerbsContext tests ──────────────────────────────────────────────────

TEST(RdmaVerbs, OpenNonexistentDeviceFails) {
    VerbsContext ctx;
    auto res = ctx.open("totally_bogus_device_99");
    // Either "not found" or "No RDMA devices found" (if no HCA present)
    EXPECT_FALSE(res.has_value());
    EXPECT_FALSE(ctx.is_open());
}

TEST(RdmaVerbs, CreatePdWithoutOpenFails) {
    VerbsContext ctx;
    auto res = ctx.create_pd();
    EXPECT_FALSE(res.has_value());
    EXPECT_NE(res.error().find("not open"), std::string::npos);
}

TEST(RdmaVerbs, AllocMrWithoutPdFails) {
    VerbsContext ctx;
    uint8_t buf[64];
    auto res = ctx.alloc_mr(buf, sizeof(buf), 0);
    EXPECT_FALSE(res.has_value());
    EXPECT_NE(res.error().find("PD not created"), std::string::npos);
}

TEST(RdmaVerbs, AllocMrNullBufferFails) {
    VerbsContext ctx;
    // Even if PD were created, null buffer should fail
    auto res = ctx.alloc_mr(nullptr, 0, 0);
    EXPECT_FALSE(res.has_value());
}

TEST(RdmaVerbs, CreateCqWithoutOpenFails) {
    VerbsContext ctx;
    auto res = ctx.create_cq(256);
    EXPECT_FALSE(res.has_value());
    EXPECT_NE(res.error().find("not open"), std::string::npos);
}

TEST(RdmaVerbs, CloseWithoutOpenIsNoOp) {
    VerbsContext ctx;
    ctx.close(); // should not crash
    EXPECT_FALSE(ctx.is_open());
}

TEST(RdmaVerbs, MoveSemantics) {
    VerbsContext a;
    EXPECT_FALSE(a.is_open());
    VerbsContext b(std::move(a));
    EXPECT_FALSE(b.is_open());
    EXPECT_FALSE(a.is_open()); // NOLINT
}

// ─── MemoryRegionManager tests ──────────────────────────────────────────

TEST(RdmaMemRegion, RegisterNullBufferFails) {
    VerbsContext ctx;
    MemoryRegionManager mgr(ctx);
    auto res = mgr.register_region(nullptr, 0);
    EXPECT_FALSE(res.has_value());
    EXPECT_NE(res.error().find("Invalid"), std::string::npos);
}

TEST(RdmaMemRegion, RegisterWithoutPdFails) {
    VerbsContext ctx;
    MemoryRegionManager mgr(ctx);
    uint8_t buf[128];
    auto res = mgr.register_region(buf, sizeof(buf));
    EXPECT_FALSE(res.has_value());
    EXPECT_NE(res.error().find("Protection domain"), std::string::npos);
}

TEST(RdmaMemRegion, DeregisterNonexistentFails) {
    VerbsContext ctx;
    MemoryRegionManager mgr(ctx);
    auto res = mgr.deregister(9999);
    EXPECT_FALSE(res.has_value());
    EXPECT_NE(res.error().find("not found"), std::string::npos);
}

TEST(RdmaMemRegion, GetInfoNonexistentReturnsNull) {
    VerbsContext ctx;
    MemoryRegionManager mgr(ctx);
    EXPECT_EQ(mgr.get_info(42), nullptr);
    EXPECT_EQ(mgr.count(), 0u);
}

// ─── QueuePairManager tests ─────────────────────────────────────────────

TEST(RdmaQueuePair, CreateWithoutPdFails) {
    VerbsContext ctx;
    QueuePairManager qpm(ctx);
    auto res = qpm.create_qp(2 /* IBV_QPT_RC */, 128, 128);
    EXPECT_FALSE(res.has_value());
    EXPECT_NE(res.error().find("PD not created"), std::string::npos);
}

TEST(RdmaQueuePair, ModifyToInitNonexistentQpFails) {
    VerbsContext ctx;
    QueuePairManager qpm(ctx);
    auto res = qpm.modify_to_init(99999);
    EXPECT_FALSE(res.has_value());
    EXPECT_NE(res.error().find("not found"), std::string::npos);
}

TEST(RdmaQueuePair, PostSendNonexistentQpFails) {
    VerbsContext ctx;
    QueuePairManager qpm(ctx);
    SendWR wr = {};
    auto res = qpm.post_send(12345, wr);
    EXPECT_FALSE(res.has_value());
}

TEST(RdmaQueuePair, PostRecvNonexistentQpFails) {
    VerbsContext ctx;
    QueuePairManager qpm(ctx);
    RecvWR wr = {};
    auto res = qpm.post_recv(12345, wr);
    EXPECT_FALSE(res.has_value());
}

TEST(RdmaQueuePair, PollCqWithoutCqFails) {
    VerbsContext ctx;
    QueuePairManager qpm(ctx);
    auto res = qpm.poll_cq(0);
    EXPECT_FALSE(res.has_value());
    EXPECT_NE(res.error().find("CQ not created"), std::string::npos);
}

TEST(RdmaQueuePair, InitialQpCountIsZero) {
    VerbsContext ctx;
    QueuePairManager qpm(ctx);
    EXPECT_EQ(qpm.qp_count(), 0u);
}

// ─── TensorRdma tests ───────────────────────────────────────────────────

TEST(RdmaTensor, WriteTensorNotConnectedFails) {
    VerbsContext ctx;
    MemoryRegionManager mr_mgr(ctx);
    QueuePairManager qp_mgr(ctx);
    TensorRdma tensor(ctx, qp_mgr, mr_mgr);

    auto res = tensor.write_tensor(1, 0x1000, 0xABCD, 4096);
    EXPECT_FALSE(res.has_value());
    EXPECT_NE(res.error().find("Not connected"), std::string::npos);
    EXPECT_FALSE(tensor.connected());
}

TEST(RdmaTensor, ReadTensorNotConnectedFails) {
    VerbsContext ctx;
    MemoryRegionManager mr_mgr(ctx);
    QueuePairManager qp_mgr(ctx);
    TensorRdma tensor(ctx, qp_mgr, mr_mgr);

    auto res = tensor.read_tensor(0x1000, 0xABCD, 1, 4096);
    EXPECT_FALSE(res.has_value());
    EXPECT_NE(res.error().find("Not connected"), std::string::npos);
}

TEST(RdmaTensor, ConnectToUnresolvableHostFails) {
    VerbsContext ctx;
    MemoryRegionManager mr_mgr(ctx);
    QueuePairManager qp_mgr(ctx);
    TensorRdma tensor(ctx, qp_mgr, mr_mgr);

    // Will fail at QP creation (no PD) before even reaching TCP
    auto res = tensor.connect("nonexistent.invalid.host.test", 18515);
    EXPECT_FALSE(res.has_value());
}

TEST(RdmaTensor, DefaultState) {
    VerbsContext ctx;
    MemoryRegionManager mr_mgr(ctx);
    QueuePairManager qp_mgr(ctx);
    TensorRdma tensor(ctx, qp_mgr, mr_mgr);

    EXPECT_FALSE(tensor.connected());
    EXPECT_EQ(tensor.local_qp_num(), 0u);
}
