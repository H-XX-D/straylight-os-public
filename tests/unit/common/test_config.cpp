// tests/unit/common/test_config.cpp
#include <gtest/gtest.h>
#include <straylight/config.h>
#include <fstream>
#include <filesystem>

using namespace straylight;

class ConfigTest : public ::testing::Test {
protected:
    std::filesystem::path tmp_dir;

    void SetUp() override {
        tmp_dir = std::filesystem::temp_directory_path() / "straylight_test_config";
        std::filesystem::create_directories(tmp_dir);
    }

    void TearDown() override {
        std::filesystem::remove_all(tmp_dir);
    }

    void write_file(const std::string& name, const std::string& content) {
        std::ofstream f(tmp_dir / name);
        f << content;
    }
};

TEST_F(ConfigTest, LoadValidJson) {
    write_file("test.json", R"({"name": "straylight", "version": 1})");
    auto result = Config::load(tmp_dir / "test.json");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().get<std::string>("name"), "straylight");
    EXPECT_EQ(result.value().get<int>("version"), 1);
}

TEST_F(ConfigTest, LoadMissingFileReturnsError) {
    auto result = Config::load(tmp_dir / "nonexistent.json");
    ASSERT_FALSE(result.has_value());
}

TEST_F(ConfigTest, LoadInvalidJsonReturnsError) {
    write_file("bad.json", "not json {{{");
    auto result = Config::load(tmp_dir / "bad.json");
    ASSERT_FALSE(result.has_value());
}

TEST_F(ConfigTest, GetWithDefaultReturnsFallback) {
    write_file("test.json", R"({"name": "straylight"})");
    auto result = Config::load(tmp_dir / "test.json");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().get<int>("missing_key", 42), 42);
}

TEST_F(ConfigTest, NestedAccess) {
    write_file("test.json", R"({"display": {"width": 1920, "height": 1080}})");
    auto result = Config::load(tmp_dir / "test.json");
    ASSERT_TRUE(result.has_value());
    auto& cfg = result.value();
    EXPECT_EQ(cfg.get<int>("display.width"), 1920);
    EXPECT_EQ(cfg.get<int>("display.height"), 1080);
}
