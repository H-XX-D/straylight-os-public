// tests/integration/test_boot_sequence.cpp
// Integration test for the full boot state machine transitions.
// Tests the state file transitions without requiring real services.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

class BootSequenceTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = fs::temp_directory_path() / "straylight_boot_test";
        fs::create_directories(tmp_dir_);
        state_file_ = (tmp_dir_ / "state").string();
    }

    void TearDown() override {
        fs::remove_all(tmp_dir_);
    }

    void write_state(const std::string& state) {
        std::string tmp = state_file_ + ".tmp";
        std::ofstream f(tmp, std::ios::trunc);
        f << state;
        f.close();
        std::rename(tmp.c_str(), state_file_.c_str());
    }

    std::string read_state() {
        std::ifstream f(state_file_);
        std::string s;
        std::getline(f, s);
        return s;
    }

    /// Simulate what straylight-firstboot does
    bool run_firstboot() {
        std::string state = read_state();
        if (state != "firstboot") return true;  // idempotent exit 0
        write_state("oobe");
        return true;
    }

    /// Simulate straylight-oobe-check
    bool oobe_check() {
        return read_state() == "oobe";
    }

    /// Simulate OOBE completion
    void oobe_complete() {
        write_state("wizard");
    }

    /// Simulate straylight-wizard-check
    bool wizard_check() {
        return read_state() == "wizard";
    }

    /// Simulate wizard completion
    void wizard_complete() {
        write_state("complete");
    }

    fs::path tmp_dir_;
    std::string state_file_;
};

TEST_F(BootSequenceTest, FullBootSequence) {
    // 1. State = firstboot → firstboot script → assert state = oobe
    write_state("firstboot");
    EXPECT_TRUE(run_firstboot());
    EXPECT_EQ(read_state(), "oobe");

    // 2. State = oobe → OOBE check passes
    EXPECT_TRUE(oobe_check());

    // 3. OOBE completes → state = wizard
    oobe_complete();
    EXPECT_EQ(read_state(), "wizard");

    // 4. State = wizard → wizard check passes
    EXPECT_TRUE(wizard_check());

    // 5. Wizard completes → state = complete
    wizard_complete();
    EXPECT_EQ(read_state(), "complete");

    // 6. State = complete → neither check passes
    EXPECT_FALSE(oobe_check());
    EXPECT_FALSE(wizard_check());
}

TEST_F(BootSequenceTest, FirstbootIdempotent) {
    // Running firstboot with state = oobe should not regress
    write_state("oobe");
    EXPECT_TRUE(run_firstboot());
    EXPECT_EQ(read_state(), "oobe");
}

TEST_F(BootSequenceTest, FirstbootWithCompleteState) {
    write_state("complete");
    EXPECT_TRUE(run_firstboot());
    EXPECT_EQ(read_state(), "complete");
}

TEST_F(BootSequenceTest, OobeCheckFailsOnComplete) {
    write_state("complete");
    EXPECT_FALSE(oobe_check());
}

TEST_F(BootSequenceTest, WizardCheckFailsOnOobe) {
    write_state("oobe");
    EXPECT_FALSE(wizard_check());
}
