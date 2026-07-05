// tools/env/env_manager.h
// Environment variable manager — persistent env vars with profiles.
#pragma once

#include <straylight/result.h>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace straylight {

struct EnvProfile {
    std::string name;
    std::map<std::string, std::string> vars;
};

struct EnvDiff {
    struct Entry { std::string key, left, right; };
    std::vector<Entry> changed;
    std::vector<Entry> only_left;
    std::vector<Entry> only_right;
};

class EnvManager {
public:
    explicit EnvManager(const std::filesystem::path& config_dir = "");

    Result<void, std::string> set(const std::string& key, const std::string& value,
                                   const std::string& profile = "");
    Result<std::string, std::string> get(const std::string& key,
                                          const std::string& profile = "") const;
    std::map<std::string, std::string> list(const std::string& profile = "") const;
    std::vector<std::string> list_profiles() const;
    Result<void, std::string> switch_profile(const std::string& profile);
    std::string active_profile() const;
    EnvDiff diff(const std::string& p1, const std::string& p2) const;
    std::string source_snippet(const std::string& profile = "") const;
    Result<int, std::string> run_with_profile(const std::string& cmd, const std::string& profile);
    Result<EnvProfile, std::string> load_profile(const std::string& name) const;
    Result<void, std::string> save_profile(const EnvProfile& profile) const;
    Result<void, std::string> delete_profile(const std::string& name);

private:
    std::filesystem::path config_dir_;
    std::string active_profile_ = "default";
    std::filesystem::path profile_path(const std::string& name) const;
    std::filesystem::path state_path() const;
    void load_state();
    void save_state() const;
};

} // namespace straylight
