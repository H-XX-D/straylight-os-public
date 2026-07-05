// tests/unit/compositor/test_ipc_compositor.cpp
// Tests CompositorEvent/CompositorCommand JSON serialization round-trip
// without needing a live socket or wlroots.
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::json;

// Mirror of the IPC event types from compositor/ipc.h
enum class EventType {
    WindowMapped, WindowUnmapped,
    OutputAdded, OutputRemoved,
    SessionLocked, SessionUnlocked
};

struct CompositorEvent {
    EventType   type;
    std::string app_id;
    std::string title;
    std::string output_name;
    int         width  = 0;
    int         height = 0;
};

json serialize(const CompositorEvent& e) {
    switch (e.type) {
        case EventType::WindowMapped:
            return {{"type","WindowMapped"},{"app_id",e.app_id},{"title",e.title}};
        case EventType::WindowUnmapped:
            return {{"type","WindowUnmapped"},{"app_id",e.app_id}};
        case EventType::OutputAdded:
            return {{"type","OutputAdded"},{"name",e.output_name},
                    {"width",e.width},{"height",e.height}};
        case EventType::OutputRemoved:
            return {{"type","OutputRemoved"},{"name",e.output_name}};
        case EventType::SessionLocked:
            return {{"type","SessionLocked"}};
        case EventType::SessionUnlocked:
            return {{"type","SessionUnlocked"}};
    }
    return {};
}

TEST(CompositorIpc, WindowMappedSerializes) {
    CompositorEvent e{EventType::WindowMapped, "org.kde.konsole", "Terminal"};
    auto j = serialize(e);
    EXPECT_EQ(j["type"],   "WindowMapped");
    EXPECT_EQ(j["app_id"], "org.kde.konsole");
    EXPECT_EQ(j["title"],  "Terminal");
}

TEST(CompositorIpc, WindowUnmappedSerializes) {
    CompositorEvent e{EventType::WindowUnmapped, "org.kde.konsole"};
    auto j = serialize(e);
    EXPECT_EQ(j["type"],   "WindowUnmapped");
    EXPECT_EQ(j["app_id"], "org.kde.konsole");
    EXPECT_FALSE(j.contains("title"));
}

TEST(CompositorIpc, OutputAddedSerializes) {
    CompositorEvent e{EventType::OutputAdded};
    e.output_name = "DP-1";
    e.width = 2560; e.height = 1440;
    auto j = serialize(e);
    EXPECT_EQ(j["type"],   "OutputAdded");
    EXPECT_EQ(j["name"],   "DP-1");
    EXPECT_EQ(j["width"],  2560);
    EXPECT_EQ(j["height"], 1440);
}

TEST(CompositorIpc, SessionLockedHasNoPayload) {
    CompositorEvent e{EventType::SessionLocked};
    auto j = serialize(e);
    EXPECT_EQ(j["type"], "SessionLocked");
    EXPECT_EQ(j.size(),  1u);
}

TEST(CompositorIpc, JsonRoundTripOutputRemoved) {
    CompositorEvent e{EventType::OutputRemoved};
    e.output_name = "HDMI-A-1";
    auto serialized = serialize(e).dump();
    auto parsed = json::parse(serialized);
    EXPECT_EQ(parsed["type"], "OutputRemoved");
    EXPECT_EQ(parsed["name"], "HDMI-A-1");
}
