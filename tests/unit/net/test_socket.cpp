#include <gtest/gtest.h>
#include <straylight/net/socket.h>
#include <thread>
#include <chrono>

using namespace straylight::net;
using namespace std::chrono_literals;

TEST(UdpSocketTest, SendAndReceive) {
    UdpSocket server;
    auto bind_result = server.bind("127.0.0.1", 0);
    ASSERT_TRUE(bind_result.has_value()) << bind_result.error();
    auto port = server.local_port();

    std::thread sender([port]() {
        std::this_thread::sleep_for(50ms);
        UdpSocket client;
        client.send_to("127.0.0.1", port, "hello", 5);
    });

    uint8_t buf[64];
    auto result = server.recv_from(buf, sizeof(buf), 1000);
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result.value().bytes_received, 5u);
    EXPECT_EQ(std::string(reinterpret_cast<char*>(buf), 5), "hello");

    sender.join();
}
