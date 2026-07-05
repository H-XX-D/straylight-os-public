// tests/unit/shell/test_theme_engine.cpp
// Unit tests for the shell theme engine
#include <gtest/gtest.h>

#include "themes/theme_engine.h"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <fstream>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using namespace straylight::shell;

class ThemeEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create a temporary directory for test theme files
        tmp_dir_ = fs::temp_directory_path() / "straylight_theme_test";
        fs::create_directories(tmp_dir_);

        // Create ImGui context for style testing
        ImGui::CreateContext();
    }

    void TearDown() override {
        ImGui::DestroyContext();
        fs::remove_all(tmp_dir_);
    }

    void write_theme_file(const std::string& name,
                          const nlohmann::json& content) {
        std::ofstream f(tmp_dir_ / (name + ".json"));
        f << content.dump(2);
    }

    fs::path tmp_dir_;
};

TEST_F(ThemeEngineTest, LoadsDefaultThemeWithoutError) {
    nlohmann::json theme = {
        {"name", "default"},
        {"colors", {
            {"bg", "#1E1E2E"},
            {"fg", "#CDD6F4"},
            {"accent", "#B4BEFE"},
            {"panel", "#313244"}
        }},
        {"font_size", 16.0},
        {"corner_radius", 4.0},
        {"icon_theme", "straylight-icons"}
    };
    write_theme_file("default", theme);

    ThemeEngine engine;
    auto result = engine.load((tmp_dir_ / "default.json").string());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().name, "default");
}

TEST_F(ThemeEngineTest, ApplySetsWindowBgToSpecColor) {
    nlohmann::json theme = {
        {"name", "test"},
        {"colors", {
            {"bg", "#1E1E2E"},
            {"fg", "#CDD6F4"},
            {"accent", "#B4BEFE"},
            {"panel", "#313244"}
        }},
        {"font_size", 16.0},
        {"corner_radius", 4.0}
    };
    write_theme_file("test", theme);

    ThemeEngine engine;
    auto result = engine.load((tmp_dir_ / "test.json").string());
    ASSERT_TRUE(result.has_value());

    ImGuiStyle style;
    engine.apply(style);

    // Verify WindowBg was set (bg = #1E1E2E → R=0x1E, G=0x1E, B=0x2E)
    const ImVec4& bg = style.Colors[ImGuiCol_WindowBg];
    EXPECT_NEAR(bg.x, 0x1E / 255.0f, 0.01f);  // R
    EXPECT_NEAR(bg.y, 0x1E / 255.0f, 0.01f);  // G
    EXPECT_NEAR(bg.z, 0x2E / 255.0f, 0.01f);  // B
}

TEST_F(ThemeEngineTest, MissingKeyFallsBackToDefault) {
    // Theme with missing "colors" key
    nlohmann::json theme = {
        {"name", "partial"},
        {"font_size", 20.0}
    };
    write_theme_file("partial", theme);

    ThemeEngine engine;
    auto result = engine.load((tmp_dir_ / "partial.json").string());
    ASSERT_TRUE(result.has_value());

    // Should use default color values, not crash
    ImGuiStyle style;
    EXPECT_NO_FATAL_FAILURE(engine.apply(style));
}

TEST_F(ThemeEngineTest, MalformedJsonReturnsParseError) {
    // Write invalid JSON
    std::ofstream f(tmp_dir_ / "bad.json");
    f << "{ invalid json }}}";
    f.close();

    ThemeEngine engine;
    auto result = engine.load((tmp_dir_ / "bad.json").string());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), straylight::SLErrorCode::ParseError);
}

TEST_F(ThemeEngineTest, NonexistentFileReturnsNotFound) {
    ThemeEngine engine;
    auto result = engine.load("/nonexistent/theme.json");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), straylight::SLErrorCode::NotFound);
}
