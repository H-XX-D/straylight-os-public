// tests/unit/common/test_ipc.cpp
#include <gtest/gtest.h>
#include <straylight/ipc.h>

#include <filesystem>
#include <thread>
#include <chrono>

using namespace straylight;
using namespace std::chrono_literals;

class IpcTest : public ::testing::Test {
protected:
    std::filesystem::path sock_path;

    void SetUp() override {
        sock_path = std::filesystem::temp_directory_path() / "straylight_test.sock";
        std::filesystem::remove(sock_path);
    }

    void TearDown() override {
        std::filesystem::remove(sock_path);
    }
};

TEST_F(IpcTest, ServerAcceptsClient) {
    IpcServer server;
    auto bind_result = server.bind(sock_path.string());
    ASSERT_TRUE(bind_result.has_value()) << bind_result.error();

    std::thread client_thread([&]() {
        std::this_thread::sleep_for(50ms);
        IpcClient client;
        auto conn = client.connect(sock_path.string());
        ASSERT_TRUE(conn.has_value()) << conn.error();
    });

    auto conn = server.accept(1000);  // 1s timeout
    ASSERT_TRUE(conn.has_value()) << conn.error();

    client_thread.join();
}

TEST_F(IpcTest, SendAndReceiveMessage) {
    IpcServer server;
    server.bind(sock_path.string());

    std::string received;
    std::thread client_thread([&]() {
        std::this_thread::sleep_for(50ms);
        IpcClient client;
        client.connect(sock_path.string());
        client.send(R"({"type":"ping"})");
        auto resp = client.receive();
        if (resp.has_value()) received = resp.value();
    });

    auto conn = server.accept(1000);
    ASSERT_TRUE(conn.has_value());

    auto msg = conn.value()->receive();
    ASSERT_TRUE(msg.has_value());
    EXPECT_EQ(msg.value(), R"({"type":"ping"})");

    conn.value()->send(R"({"type":"pong"})");

    client_thread.join();
    EXPECT_EQ(received, R"({"type":"pong"})");
}
