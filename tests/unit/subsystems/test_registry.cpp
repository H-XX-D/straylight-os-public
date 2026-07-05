#include <gtest/gtest.h>
#include "store.h"

using namespace straylight;

TEST(RegistryStore, SetAndGet) {
    Store store;
    store.set("network.hostname", "straylight-dev");
    auto v = store.get("network.hostname");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, "straylight-dev");
}

TEST(RegistryStore, GetMissingKey) {
    Store store;
    EXPECT_FALSE(store.get("does.not.exist").has_value());
}

TEST(RegistryStore, Watch) {
    Store store;
    std::string last_value;
    store.watch("ui.theme", [&](const std::string& v) { last_value = v; });
    store.set("ui.theme", "cyberpunk");
    EXPECT_EQ(last_value, "cyberpunk");
    store.set("ui.theme", "minimal");
    EXPECT_EQ(last_value, "minimal");
}

TEST(RegistryStore, JsonRoundtrip) {
    Store store;
    store.set("a.b", "1");
    store.set("a.c", "2");
    auto json_str = store.serialize();
    Store store2;
    ASSERT_TRUE(store2.deserialize(json_str).has_value());
    EXPECT_EQ(store2.get("a.b"), std::optional<std::string>{"1"});
    EXPECT_EQ(store2.get("a.c"), std::optional<std::string>{"2"});
}

TEST(RegistryStore, Delete) {
    Store store;
    store.set("tmp.key", "val");
    store.del("tmp.key");
    EXPECT_FALSE(store.get("tmp.key").has_value());
}
