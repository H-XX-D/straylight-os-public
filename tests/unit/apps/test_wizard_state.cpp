// tests/unit/apps/test_wizard_state.cpp
// Unit tests for the wizard firstboot state utilities
#include <gtest/gtest.h>

#include "firstboot.h"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace straylight::wizard;

class WizardStateTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = fs::temp_directory_path() / "straylight_wizard_test";
        fs::create_directories(tmp_dir_);
        state_file_ = (tmp_dir_ / "state").string();
    }

    void TearDown() override {
        fs::remove_all(tmp_dir_);
    }

    void write_state(const std::string& state) {
        std::ofstream f(state_file_, std::ios::trunc);
        f << state;
    }

    std::string read_state() {
        std::ifstream f(state_file_);
        std::string s;
        std::getline(f, s);
        return s;
    }

    fs::path tmp_dir_;
    std::string state_file_;
};

TEST_F(WizardStateTest, IsFirstbootReturnsTrueWhenWizard) {
    write_state("wizard");
    EXPECT_TRUE(is_firstboot(state_file_));
}

TEST_F(WizardStateTest, IsFirstbootReturnsFalseWhenComplete) {
    write_state("complete");
    EXPECT_FALSE(is_firstboot(state_file_));
}

TEST_F(WizardStateTest, IsFirstbootReturnsFalseWhenFileAbsent) {
    std::string nonexistent = (tmp_dir_ / "nonexistent").string();
    EXPECT_FALSE(is_firstboot(nonexistent));
}

TEST_F(WizardStateTest, MarkCompleteOverwritesState) {
    write_state("wizard");
    EXPECT_EQ(read_state(), "wizard");

    mark_complete(state_file_);
    EXPECT_EQ(read_state(), "complete");
}

TEST_F(WizardStateTest, MarkCompleteIsAtomic) {
    mark_complete(state_file_);

    // Temp file should not exist after successful write
    EXPECT_FALSE(fs::exists(state_file_ + ".tmp"));
    // Final file should exist
    EXPECT_TRUE(fs::exists(state_file_));
    EXPECT_EQ(read_state(), "complete");
}

TEST_F(WizardStateTest, ReadBootStateReturnsEmptyForMissingFile) {
    std::string result = read_boot_state(
        (tmp_dir_ / "nonexistent").string());
    EXPECT_TRUE(result.empty());
}

TEST_F(WizardStateTest, ReadBootStateReturnsContent) {
    write_state("oobe");
    EXPECT_EQ(read_boot_state(state_file_), "oobe");
}
