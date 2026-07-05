#include <gtest/gtest.h>
#include <straylight/net/transport.h>
#include <straylight/net/protocol.h>
#include <straylight/types.h>

using namespace straylight;
using namespace straylight::net;

TEST(TransportTest, TensorHeaderSize) {
    EXPECT_EQ(sizeof(TensorHeader), 80u);
}

TEST(TransportTest, TensorHeaderMagic) {
    TensorHeader hdr{};
    EXPECT_EQ(hdr.magic, 0x53544C54u);
    EXPECT_EQ(hdr.version, 1u);
}
