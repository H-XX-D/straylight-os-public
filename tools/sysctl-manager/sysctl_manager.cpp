// tools/sysctl-manager/sysctl_manager.cpp
#include "sysctl_manager.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace straylight {

namespace fs = std::filesystem;

SysctlManager::SysctlManager(const fs::path& profile_dir)
    : profile_dir_(profile_dir) {}

fs::path SysctlManager::key_to_proc_path(const std::string& key) const {
    std::string path_str = key;
    std::replace(path_str.begin(), path_str.end(), '.', '/');
    return fs::path("/proc/sys") / path_str;
}

std::string SysctlManager::proc_path_to_key(const fs::path& path) const {
    std::string rel = path.string().substr(std::string("/proc/sys/").size());
    std::replace(rel.begin(), rel.end(), '/', '.');
    return rel;
}

std::string SysctlManager::read_file(const fs::path& path) const {
    std::ifstream ifs(path);
    if (!ifs) return "";
    std::string content;
    std::getline(ifs, content);
    // Trim whitespace
    while (!content.empty() && (content.back() == '\n' || content.back() == '\r' ||
                                 content.back() == ' ' || content.back() == '\t')) {
        content.pop_back();
    }
    return content;
}

bool SysctlManager::write_file(const fs::path& path, const std::string& content) const {
    std::ofstream ofs(path);
    if (!ofs) return false;
    ofs << content << "\n";
    return ofs.good();
}

Result<std::string, std::string> SysctlManager::get(const std::string& key) const {
    auto path = key_to_proc_path(key);
    if (!fs::exists(path)) {
        return Result<std::string, std::string>::error("Key not found: " + key);
    }
    std::string val = read_file(path);
    if (val.empty() && !fs::exists(path)) {
        return Result<std::string, std::string>::error("Cannot read: " + key);
    }
    return Result<std::string, std::string>::ok(val);
}

Result<void, std::string> SysctlManager::set(const std::string& key, const std::string& value) {
    auto path = key_to_proc_path(key);
    if (!fs::exists(path)) {
        return Result<void, std::string>::error("Key not found: " + key);
    }
    if (!write_file(path, value)) {
        return Result<void, std::string>::error(
            "Cannot write to " + key + " (permission denied?)");
    }
    return Result<void, std::string>::ok();
}

std::vector<std::string> SysctlManager::list_profiles() const {
    std::vector<std::string> profiles;
    if (!fs::exists(profile_dir_)) return profiles;

    for (const auto& entry : fs::directory_iterator(profile_dir_)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        if (ext == ".json" || ext == ".conf") {
            profiles.push_back(entry.path().stem().string());
        }
    }
    std::sort(profiles.begin(), profiles.end());
    return profiles;
}

Result<SysctlProfile, std::string> SysctlManager::load_profile(
    const std::string& name) const {
    fs::path path = profile_dir_ / (name + ".json");
    if (!fs::exists(path)) {
        path = profile_dir_ / (name + ".conf");
        if (!fs::exists(path)) {
            return Result<SysctlProfile, std::string>::error(
                "Profile not found: " + name);
        }
    }

    std::ifstream ifs(path);
    if (!ifs) {
        return Result<SysctlProfile, std::string>::error("Cannot read profile: " + name);
    }

    try {
        nlohmann::json j;
        ifs >> j;

        SysctlProfile profile;
        profile.name = j.value("name", name);
        profile.description = j.value("description", "");

        if (j.contains("params") && j["params"].is_object()) {
            for (auto& [k, v] : j["params"].items()) {
                profile.params[k] = v.get<std::string>();
            }
        }

        return Result<SysctlProfile, std::string>::ok(std::move(profile));
    } catch (const std::exception& e) {
        return Result<SysctlProfile, std::string>::error(
            "Parse error in profile '" + name + "': " + e.what());
    }
}

Result<void, std::string> SysctlManager::save_profile(
    const std::string& name, const std::string& description,
    const std::vector<std::string>& keys) {

    if (!fs::exists(profile_dir_)) {
        std::error_code ec;
        fs::create_directories(profile_dir_, ec);
        if (ec) {
            return Result<void, std::string>::error(
                "Cannot create profile directory: " + ec.message());
        }
    }

    SysctlProfile profile;
    profile.name = name;
    profile.description = description;

    if (keys.empty()) {
        // Save all readable sysctl values
        profile.params = read_all();
    } else {
        for (const auto& key : keys) {
            auto val = get(key);
            if (val.has_value()) {
                profile.params[key] = val.value();
            }
        }
    }

    nlohmann::json j;
    j["name"] = profile.name;
    j["description"] = profile.description;
    j["params"] = nlohmann::json::object();
    for (const auto& [k, v] : profile.params) {
        j["params"][k] = v;
    }

    fs::path path = profile_dir_ / (name + ".json");
    std::ofstream ofs(path);
    if (!ofs) {
        return Result<void, std::string>::error("Cannot write profile: " + path.string());
    }
    ofs << j.dump(4) << "\n";
    return Result<void, std::string>::ok();
}

Result<void, std::string> SysctlManager::apply(const std::string& profile_name) {
    auto profile_res = load_profile(profile_name);
    if (!profile_res.has_value()) {
        return Result<void, std::string>::error(profile_res.error());
    }

    const auto& profile = profile_res.value();

    // Save current state for rollback
    nlohmann::json rollback;
    rollback["params"] = nlohmann::json::object();
    for (const auto& [key, _] : profile.params) {
        auto current = get(key);
        if (current.has_value()) {
            rollback["params"][key] = current.value();
        }
    }
    std::ofstream rofs(ROLLBACK_FILE);
    if (rofs) {
        rofs << rollback.dump(4) << "\n";
    }

    // Apply all parameters
    int applied = 0;
    int failed = 0;
    std::string errors;
    for (const auto& [key, value] : profile.params) {
        auto res = set(key, value);
        if (res.has_value()) {
            applied++;
        } else {
            failed++;
            errors += "  " + key + ": " + res.error() + "\n";
        }
    }

    if (failed > 0) {
        return Result<void, std::string>::error(
            "Applied " + std::to_string(applied) + "/" +
            std::to_string(applied + failed) + " parameters. Errors:\n" + errors);
    }

    return Result<void, std::string>::ok();
}

Result<SysctlDiff, std::string> SysctlManager::diff(const std::string& profile_name) const {
    auto profile_res = load_profile(profile_name);
    if (!profile_res.has_value()) {
        return Result<SysctlDiff, std::string>::error(profile_res.error());
    }

    const auto& profile = profile_res.value();
    SysctlDiff result;

    for (const auto& [key, target_val] : profile.params) {
        auto current = get(key);
        if (!current.has_value()) {
            result.added.push_back({key, "(not present)", target_val});
        } else if (current.value() != target_val) {
            result.changed.push_back({key, current.value(), target_val});
        }
    }

    return Result<SysctlDiff, std::string>::ok(std::move(result));
}

Result<SysctlDiff, std::string> SysctlManager::diff_default() const {
    if (!fs::exists(ROLLBACK_FILE)) {
        return Result<SysctlDiff, std::string>::error("No rollback state saved");
    }

    std::ifstream ifs(ROLLBACK_FILE);
    if (!ifs) {
        return Result<SysctlDiff, std::string>::error("Cannot read rollback file");
    }

    try {
        nlohmann::json j;
        ifs >> j;

        SysctlDiff result;
        if (j.contains("params") && j["params"].is_object()) {
            for (auto& [key, saved_val] : j["params"].items()) {
                std::string saved = saved_val.get<std::string>();
                auto current = get(key);
                if (current.has_value() && current.value() != saved) {
                    result.changed.push_back({key, current.value(), saved});
                }
            }
        }
        return Result<SysctlDiff, std::string>::ok(std::move(result));
    } catch (const std::exception& e) {
        return Result<SysctlDiff, std::string>::error(
            std::string("Parse error: ") + e.what());
    }
}

Result<void, std::string> SysctlManager::rollback() {
    if (!fs::exists(ROLLBACK_FILE)) {
        return Result<void, std::string>::error("No rollback state saved");
    }

    std::ifstream ifs(ROLLBACK_FILE);
    if (!ifs) {
        return Result<void, std::string>::error("Cannot read rollback file");
    }

    try {
        nlohmann::json j;
        ifs >> j;

        int restored = 0;
        if (j.contains("params") && j["params"].is_object()) {
            for (auto& [key, val] : j["params"].items()) {
                auto res = set(key, val.get<std::string>());
                if (res.has_value()) restored++;
            }
        }

        // Remove rollback file
        fs::remove(ROLLBACK_FILE);

        return Result<void, std::string>::ok();
    } catch (const std::exception& e) {
        return Result<void, std::string>::error(
            std::string("Rollback failed: ") + e.what());
    }
}

std::map<std::string, std::string> SysctlManager::read_all(const std::string& prefix) const {
    std::map<std::string, std::string> result;
    fs::path base = "/proc/sys";

    if (!prefix.empty()) {
        std::string subpath = prefix;
        std::replace(subpath.begin(), subpath.end(), '.', '/');
        base = base / subpath;
    }

    if (!fs::exists(base)) return result;

    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(base, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (!it->is_regular_file()) continue;

        std::string key = proc_path_to_key(it->path());
        std::string val = read_file(it->path());
        if (!val.empty()) {
            result[key] = val;
        }
    }

    return result;
}

} // namespace straylight
