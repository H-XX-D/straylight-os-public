// tests/unit/shell/test_top_bar.cpp
// Unit tests for the shell top bar and clock widget
#include <gtest/gtest.h>

#include "panels/top_bar.h"

#include <imgui.h>

#include <string>
#include <regex>

using namespace straylight::shell;

class TopBarTest : public ::testing::Test {
protected:
    void SetUp() override {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(1920, 1080);
        // Allocate font atlas to avoid assert
        unsigned char* pixels;
        int w, h;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    }

    void TearDown() override {
        ImGui::DestroyContext();
    }
};

TEST_F(TopBarTest, ClockFormatsTimeAsHHMM) {
    ClockWidget clock;
    std::string time_str = clock.formatted_time();

    // Must match HH:MM format
    std::regex pattern(R"(\d{2}:\d{2})");
    EXPECT_TRUE(std::regex_match(time_str, pattern))
        << "Clock output '" << time_str << "' does not match HH:MM";
}

TEST_F(TopBarTest, ClockHoursInValidRange) {
    ClockWidget clock;
    std::string time_str = clock.formatted_time();

    int hours = std::stoi(time_str.substr(0, 2));
    int mins  = std::stoi(time_str.substr(3, 2));

    EXPECT_GE(hours, 0);
    EXPECT_LE(hours, 23);
    EXPECT_GE(mins, 0);
    EXPECT_LE(mins, 59);
}

TEST_F(TopBarTest, RenderProducesDrawCommands) {
    TopBar bar;

    ImGui::NewFrame();
    bar.render(1920);
    ImGui::Render();

    ImDrawData* draw_data = ImGui::GetDrawData();
    ASSERT_NE(draw_data, nullptr);
    EXPECT_GT(draw_data->CmdListsCount, 0);
}

TEST_F(TopBarTest, NotificationCountUpdates) {
    TopBar bar;
    bar.set_notification_count(5);

    ImGui::NewFrame();
    bar.render(1920);
    ImGui::Render();

    // Just verify it doesn't crash with a notification count set
    SUCCEED();
}

TEST_F(TopBarTest, WorkspaceSwitcherUpdates) {
    TopBar bar;
    bar.set_workspace(2);

    ImGui::NewFrame();
    bar.render(1920);
    ImGui::Render();

    SUCCEED();
}
