// tools/env/env_manager.cpp
#include "env_manager.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

namespace straylight {

namespace fs = std::filesystem;

EnvManager::EnvManager(const fs::path& config_dir) {
    if (config_dir.empty()) {
        const char* home = std::getenv("HOME");
        config_dir_ = home ? fs::path(home) / ".config" / "straylight" / "env"
                           : fs::path("/etc/straylight/env");
    } else {
        config_dir_ = config_dir;
    }
    std::error_code ec;
    fs::create_directories(config_dir_, ec);
    load_state();
}

fs::path EnvManager::profile_path(const std::string& name) const {
    return config_dir_ / (name + ".json");
}
fs::path EnvManager::state_path() const { return config_dir_ / ".state.json"; }

void EnvManager::load_state() {
    if (!fs::exists(state_path())) return;
    std::ifstream ifs(state_path());
    if (!ifs) return;
    try { nlohmann::json j; ifs >> j; active_profile_ = j.value("active_profile", "default"); }
    catch (...) {}
}

void EnvManager::save_state() const {
    nlohmann::json j; j["active_profile"] = active_profile_;
    std::ofstream ofs(state_path());
    if (ofs) ofs << j.dump(2) << "\n";
}

Result<EnvProfile, std::string> EnvManager::load_profile(const std::string& name) const {
    fs::path p = profile_path(name);
    EnvProfile prof; prof.name = name;
    if (!fs::exists(p)) return Result<EnvProfile, std::string>::ok(prof);
    std::ifstream ifs(p);
    if (!ifs) return Result<EnvProfile, std::string>::error("Cannot read: " + p.string());
    try {
        nlohmann::json j; ifs >> j;
        prof.name = j.value("name", name);
        if (j.contains("vars") && j["vars"].is_object())
            for (auto& [k, v] : j["vars"].items()) prof.vars[k] = v.get<std::string>();
        return Result<EnvProfile, std::string>::ok(std::move(prof));
    } catch (const std::exception& e) {
        return Result<EnvProfile, std::string>::error(std::string("Parse error: ") + e.what());
    }
}

Result<void, std::string> EnvManager::save_profile(const EnvProfile& prof) const {
    nlohmann::json j; j["name"] = prof.name; j["vars"] = nlohmann::json::object();
    for (const auto& [k, v] : prof.vars) j["vars"][k] = v;
    std::ofstream ofs(profile_path(prof.name));
    if (!ofs) return Result<void, std::string>::error("Cannot write profile");
    ofs << j.dump(2) << "\n";
    return Result<void, std::string>::ok();
}

Result<void, std::string> EnvManager::set(const std::string& key, const std::string& value,
                                            const std::string& profile) {
    std::string pn = profile.empty() ? active_profile_ : profile;
    auto r = load_profile(pn);
    if (!r.has_value()) return Result<void, std::string>::error(r.error());
    auto p = r.value(); p.vars[key] = value;
    return save_profile(p);
}

Result<std::string, std::string> EnvManager::get(const std::string& key,
                                                   const std::string& profile) const {
    std::string pn = profile.empty() ? active_profile_ : profile;
    auto r = load_profile(pn);
    if (!r.has_value()) return Result<std::string, std::string>::error(r.error());
    auto it = r.value().vars.find(key);
    if (it == r.value().vars.end())
        return Result<std::string, std::string>::error("Key not found: " + key);
    return Result<std::string, std::string>::ok(it->second);
}

std::map<std::string, std::string> EnvManager::list(const std::string& profile) const {
    std::string pn = profile.empty() ? active_profile_ : profile;
    auto r = load_profile(pn);
    return r.has_value() ? r.value().vars : std::map<std::string, std::string>{};
}

std::vector<std::string> EnvManager::list_profiles() const {
    std::vector<std::string> profiles;
    if (!fs::exists(config_dir_)) return profiles;
    for (const auto& e : fs::directory_iterator(config_dir_)) {
        if (!e.is_regular_file() || e.path().extension() != ".json") continue;
        std::string stem = e.path().stem().string();
        if (stem.front() != '.') profiles.push_back(stem);
    }
    std::sort(profiles.begin(), profiles.end());
    return profiles;
}

Result<void, std::string> EnvManager::switch_profile(const std::string& profile) {
    active_profile_ = profile; save_state();
    return Result<void, std::string>::ok();
}

std::string EnvManager::active_profile() const { return active_profile_; }

EnvDiff EnvManager::diff(const std::string& p1, const std::string& p2) const {
    EnvDiff result;
    auto r1 = load_profile(p1), r2 = load_profile(p2);
    if (!r1.has_value() || !r2.has_value()) return result;
    const auto& v1 = r1.value().vars, & v2 = r2.value().vars;
    std::set<std::string> all;
    for (const auto& [k, _] : v1) all.insert(k);
    for (const auto& [k, _] : v2) all.insert(k);
    for (const auto& k : all) {
        bool in1 = v1.count(k), in2 = v2.count(k);
        if (in1 && in2) { if (v1.at(k) != v2.at(k)) result.changed.push_back({k, v1.at(k), v2.at(k)}); }
        else if (in1) result.only_left.push_back({k, v1.at(k), ""});
        else result.only_right.push_back({k, "", v2.at(k)});
    }
    return result;
}

std::string EnvManager::source_snippet(const std::string& profile) const {
    std::string pn = profile.empty() ? active_profile_ : profile;
    auto vars = list(pn);
    std::ostringstream out;
    out << "# StrayLight env profile: " << pn << "\n";
    for (const auto& [k, v] : vars) out << "export " << k << "='" << v << "'\n";
    return out.str();
}

Result<int, std::string> EnvManager::run_with_profile(const std::string& cmd,
                                                        const std::string& profile) {
    auto vars = list(profile);
    for (const auto& [k, v] : vars) setenv(k.c_str(), v.c_str(), 1);
    int rc = std::system(cmd.c_str());
    return Result<int, std::string>::ok(WEXITSTATUS(rc));
}

Result<void, std::string> EnvManager::delete_profile(const std::string& name) {
    fs::path p = profile_path(name);
    if (!fs::exists(p)) return Result<void, std::string>::error("Not found: " + name);
    std::error_code ec; fs::remove(p, ec);
    return ec ? Result<void, std::string>::error(ec.message()) : Result<void, std::string>::ok();
}

} // namespace straylight
