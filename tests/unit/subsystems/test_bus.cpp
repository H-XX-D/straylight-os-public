// tests/unit/subsystems/test_bus.cpp
#include <gtest/gtest.h>
#include "bus_daemon.h"

using namespace straylight;

TEST(BusRegistry, RegisterAndLookup) {
    BusDaemon bus;
    auto reg = bus.register_service("org.straylight.Test", 1001);
    EXPECT_TRUE(reg.has_value());
    auto owner = bus.lookup_owner("org.straylight.Test");
    ASSERT_TRUE(owner.has_value());
    EXPECT_EQ(*owner, 1001);
}

TEST(BusRegistry, DuplicateRegisterFails) {
    BusDaemon bus;
    ASSERT_TRUE(bus.register_service("org.straylight.Test", 1001).has_value());
    auto r = bus.register_service("org.straylight.Test", 1002);
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), SLErrorCode::AlreadyExists);
}

TEST(BusRegistry, UnregisterClearsOwner) {
    BusDaemon bus;
    ASSERT_TRUE(bus.register_service("org.straylight.Test", 1001).has_value());
    bus.unregister_service("org.straylight.Test");
    EXPECT_FALSE(bus.lookup_owner("org.straylight.Test").has_value());
}

TEST(BusSignal, ForwardSignalToSubscribers) {
    BusDaemon bus;
    std::vector<std::string> received;
    bus.subscribe("org.straylight.Test", "TestSignal",
                  [&](const std::string& payload) { received.push_back(payload); });
    bus.emit("org.straylight.Test", "TestSignal", "hello");
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0], "hello");
}
