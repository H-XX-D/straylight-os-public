// tests/unit/apps/test_oobe_state.cpp
// Unit tests for the OOBE state machine
#include <gtest/gtest.h>

#include "oobe_state.h"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace straylight::oobe;

class OobeStateTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = fs::temp_directory_path() / "straylight_oobe_test";
        fs::create_directories(tmp_dir_);
        state_file_ = (tmp_dir_ / "oobe_progress.json").string();
    }

    void TearDown() override {
        fs::remove_all(tmp_dir_);
    }

    fs::path tmp_dir_;
    std::string state_file_;
};

TEST_F(OobeStateTest, NewStateStartsAtWelcome) {
    auto result = OobeState::load(state_file_);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().current(), OobeStep::kWelcome);
}

TEST_F(OobeStateTest, AdvanceSaveReloadRestoresStep) {
    {
        auto result = OobeState::load(state_file_);
        ASSERT_TRUE(result.has_value());
        auto state = std::move(result).value();
        state.advance(OobeStep::kAccount);
        state.save();
    }

    // Reload and verify
    auto result = OobeState::load(state_file_);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().current(), OobeStep::kAccount);
}

TEST_F(OobeStateTest, CorruptJsonFallsBackToWelcome) {
    // Write corrupt JSON
    std::ofstream f(state_file_);
    f << "not valid json {{{";
    f.close();

    auto result = OobeState::load(state_file_);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().current(), OobeStep::kWelcome);
}

TEST_F(OobeStateTest, SaveIsAtomic) {
    auto result = OobeState::load(state_file_);
    ASSERT_TRUE(result.has_value());
    auto state = std::move(result).value();
    state.advance(OobeStep::kNetwork);
    state.save();

    // The temp file should not exist after successful save
    EXPECT_FALSE(fs::exists(state_file_ + ".tmp"));
    // The final file should exist
    EXPECT_TRUE(fs::exists(state_file_));
}

TEST_F(OobeStateTest, AdvanceThroughAllSteps) {
    auto result = OobeState::load(state_file_);
    ASSERT_TRUE(result.has_value());
    auto state = std::move(result).value();

    state.advance(OobeStep::kAccount);
    EXPECT_EQ(state.current(), OobeStep::kAccount);

    state.advance(OobeStep::kPackageProfile);
    EXPECT_EQ(state.current(), OobeStep::kPackageProfile);

    state.advance(OobeStep::kNetwork);
    EXPECT_EQ(state.current(), OobeStep::kNetwork);

    state.advance(OobeStep::kSummary);
    EXPECT_EQ(state.current(), OobeStep::kSummary);

    state.advance(OobeStep::kDone);
    EXPECT_EQ(state.current(), OobeStep::kDone);
}

TEST_F(OobeStateTest, StepToStringRoundTrips) {
    for (auto step : {OobeStep::kWelcome, OobeStep::kAccount,
                      OobeStep::kPackageProfile, OobeStep::kNetwork,
                      OobeStep::kSummary, OobeStep::kDone}) {
        EXPECT_EQ(string_to_step(step_to_string(step)), step);
    }
}
