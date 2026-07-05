// apps/oobe/oobe_state.cpp
// OOBE state machine implementation with atomic persistence
#include "oobe_state.h"

#include <straylight/log.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <cstdio>
#include <unistd.h>
#include <fcntl.h>

namespace straylight::oobe {

namespace fs = std::filesystem;
using json = nlohmann::json;

const char* step_to_string(OobeStep step) {
    switch (step) {
        case OobeStep::kWelcome:        return "welcome";
        case OobeStep::kAccount:        return "account";
        case OobeStep::kPackageProfile: return "package_profile";
        case OobeStep::kNetwork:        return "network";
        case OobeStep::kSummary:        return "summary";
        case OobeStep::kDone:           return "done";
    }
    return "welcome";
}

OobeStep string_to_step(std::string_view s) {
    if (s == "welcome")         return OobeStep::kWelcome;
    if (s == "account")         return OobeStep::kAccount;
    if (s == "package_profile") return OobeStep::kPackageProfile;
    if (s == "network")         return OobeStep::kNetwork;
    if (s == "summary")         return OobeStep::kSummary;
    if (s == "done")            return OobeStep::kDone;
    return OobeStep::kWelcome;
}

Result<OobeState, SLError> OobeState::load(std::string_view path) {
    OobeState state;
    state.path_ = std::string(path);

    if (!fs::exists(state.path_)) {
        SL_INFO("OOBE state file not found, starting from kWelcome");
        return Result<OobeState, SLError>::ok(std::move(state));
    }

    std::ifstream file(state.path_);
    if (!file.is_open()) {
        SL_WARN("Cannot open OOBE state file, starting from kWelcome");
        return Result<OobeState, SLError>::ok(std::move(state));
    }

    try {
        json j;
        file >> j;
        std::string step_str = j.value("step", "welcome");
        state.step_ = string_to_step(step_str);
        SL_INFO("OOBE state loaded: step={}", step_str);
    } catch (const json::exception& e) {
        SL_WARN("Corrupt OOBE state file ({}), starting from kWelcome",
                e.what());
        state.step_ = OobeStep::kWelcome;
    }

    return Result<OobeState, SLError>::ok(std::move(state));
}

void OobeState::advance(OobeStep next) {
    SL_INFO("OOBE advance: {} -> {}",
            step_to_string(step_), step_to_string(next));
    step_ = next;
}

OobeStep OobeState::current() const {
    return step_;
}

void OobeState::save() const {
    // Ensure parent directory exists
    fs::path parent = fs::path(path_).parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        fs::create_directories(parent, ec);
    }

    // Atomic write: write to temp file, fsync, rename
    std::string tmp_path = path_ + ".tmp";

    json j;
    j["step"] = step_to_string(step_);

    std::ofstream file(tmp_path, std::ios::trunc);
    if (!file.is_open()) {
        SL_ERROR("Failed to write OOBE state to {}", tmp_path);
        return;
    }

    file << j.dump(2);
    file.flush();

    // fsync the file
    int fd = open(tmp_path.c_str(), O_RDONLY);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }

    file.close();

    // Atomic rename
    if (std::rename(tmp_path.c_str(), path_.c_str()) != 0) {
        SL_ERROR("Failed to rename {} -> {}", tmp_path, path_);
    } else {
        SL_DEBUG("OOBE state saved: step={}", step_to_string(step_));
    }
}

const std::string& OobeState::path() const {
    return path_;
}

} // namespace straylight::oobe
