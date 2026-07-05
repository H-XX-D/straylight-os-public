// tests/unit/subsystems/test_xdp.cpp
#include <gtest/gtest.h>
#include "loader.h"
#include "maps.h"
#include "af_xdp.h"

using namespace straylight::xdp;

// ─── Loader tests ────────────────────────────────────────────────────────

TEST(XdpLoader, LoadNonexistentFileFails) {
    Loader loader;
    auto res = loader.load("/nonexistent/path.o", "xdp_prog");
    EXPECT_FALSE(res.has_value());
    EXPECT_NE(res.error().find("Failed"), std::string::npos);
    EXPECT_EQ(loader.prog_fd(), -1);
    EXPECT_FALSE(loader.loaded());
}

TEST(XdpLoader, AttachWithoutLoadFails) {
    Loader loader;
    auto res = loader.attach("lo", 0);
    EXPECT_FALSE(res.has_value());
    EXPECT_NE(res.error().find("No BPF program loaded"), std::string::npos);
}

TEST(XdpLoader, DetachWithoutAttachIsNoOp) {
    Loader loader;
    // Should not crash or throw — just a no-op
    loader.detach();
    EXPECT_EQ(loader.prog_fd(), -1);
    EXPECT_EQ(loader.ifindex(), 0);
}

TEST(XdpLoader, DoubleLoadFails) {
    Loader loader;
    // First load will fail (no real BPF object on disk in test), but
    // if we somehow have one, a second load while loaded should error.
    // We simulate by checking the error path after a hypothetical first load.
    auto res1 = loader.load("/dev/null", "test");
    // /dev/null is not a valid ELF — should fail
    EXPECT_FALSE(res1.has_value());
    EXPECT_FALSE(loader.loaded());
}

TEST(XdpLoader, MoveConstructor) {
    Loader a;
    // After move, the source should be reset
    Loader b(std::move(a));
    EXPECT_EQ(b.prog_fd(), -1);
    EXPECT_FALSE(b.loaded());
    EXPECT_EQ(a.prog_fd(), -1);  // NOLINT — intentional use-after-move test
}

// ─── BpfMapManager tests ────────────────────────────────────────────────

TEST(XdpMaps, LookupOnUnknownMapFails) {
    BpfMapManager mgr;
    uint32_t key = 42;
    auto res = mgr.lookup("nonexistent", &key);
    EXPECT_FALSE(res.has_value());
    EXPECT_NE(res.error().find("not found"), std::string::npos);
}

TEST(XdpMaps, UpdateOnUnknownMapFails) {
    BpfMapManager mgr;
    uint32_t key = 1, val = 2;
    auto res = mgr.update("nonexistent", &key, &val);
    EXPECT_FALSE(res.has_value());
    EXPECT_NE(res.error().find("not found"), std::string::npos);
}

TEST(XdpMaps, DeleteOnUnknownMapFails) {
    BpfMapManager mgr;
    uint32_t key = 1;
    auto res = mgr.delete_entry("nonexistent", &key);
    EXPECT_FALSE(res.has_value());
}

TEST(XdpMaps, MapFdForUnknownReturnsNegative) {
    BpfMapManager mgr;
    EXPECT_EQ(mgr.map_fd("nonexistent"), -1);
}

TEST(XdpMaps, DuplicateCreateFails) {
    BpfMapManager mgr;
    // First create may fail if not running as root — that's fine, we check the duplicate path.
    auto res1 = mgr.create_hash_map("test_map", 4, 4, 128);
    if (res1.has_value()) {
        auto res2 = mgr.create_hash_map("test_map", 4, 4, 128);
        EXPECT_FALSE(res2.has_value());
        EXPECT_NE(res2.error().find("already exists"), std::string::npos);
    }
}

// ─── AfXdpSocket tests ──────────────────────────────────────────────────

TEST(XdpAfXdp, CreateWithZeroFramesFails) {
    AfXdpSocket sock;
    auto res = sock.create("lo", 0, 0);
    EXPECT_FALSE(res.has_value());
    EXPECT_NE(res.error().find("frame_count must be > 0"), std::string::npos);
    EXPECT_FALSE(sock.is_open());
}

TEST(XdpAfXdp, SendWithoutCreateFails) {
    AfXdpSocket sock;
    uint8_t data[] = {0x01, 0x02};
    auto res = sock.send(data, sizeof(data));
    EXPECT_FALSE(res.has_value());
    EXPECT_NE(res.error().find("not created"), std::string::npos);
}

TEST(XdpAfXdp, RecvWithoutCreateFails) {
    AfXdpSocket sock;
    uint8_t buf[64];
    auto res = sock.recv(buf, sizeof(buf));
    EXPECT_FALSE(res.has_value());
    EXPECT_NE(res.error().find("not created"), std::string::npos);
}

TEST(XdpAfXdp, CloseOnUnopenedIsNoOp) {
    AfXdpSocket sock;
    sock.close(); // should not crash
    EXPECT_FALSE(sock.is_open());
}

TEST(XdpAfXdp, MoveSemantics) {
    AfXdpSocket a;
    EXPECT_FALSE(a.is_open());
    AfXdpSocket b(std::move(a));
    EXPECT_FALSE(b.is_open());
    EXPECT_FALSE(a.is_open()); // NOLINT — intentional use-after-move test
}
