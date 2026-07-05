// apps/wizard/firstboot.cpp
// Boot state file utilities
#include "firstboot.h"

#include <straylight/log.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace straylight::wizard {

namespace fs = std::filesystem;

std::string read_boot_state(const std::string& state_path) {
    if (!fs::exists(state_path)) {
        return "";
    }

    std::ifstream f(state_path);
    if (!f.is_open()) return "";

    std::string state;
    std::getline(f, state);
    return state;
}

bool is_firstboot(const std::string& state_path) {
    std::string state = read_boot_state(state_path);
    return state == "wizard";
}

void mark_complete(const std::string& state_path) {
    // Ensure parent directory exists
    fs::path parent = fs::path(state_path).parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        fs::create_directories(parent, ec);
    }

    // Atomic write: temp file + rename
    std::string tmp = state_path + ".tmp";
    std::ofstream f(tmp, std::ios::trunc);
    if (!f.is_open()) {
        SL_ERROR("Failed to write state file: {}", tmp);
        return;
    }

    f << "complete";
    f.flush();
    f.close();

    if (std::rename(tmp.c_str(), state_path.c_str()) != 0) {
        SL_ERROR("Failed to rename {} -> {}", tmp, state_path);
    } else {
        SL_INFO("Boot state set to 'complete'");
    }
}

} // namespace straylight::wizard
