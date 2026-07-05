// tools/sysctl-manager/sysctl_manager.h
// Sysctl preset manager — named kernel parameter profiles.
#pragma once

#include <straylight/result.h>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace straylight {

/// A named collection of sysctl key=value pairs.
struct SysctlProfile {
    std::string name;
    std::string description;
    std::map<std::string, std::string> params;
};

/// Difference between two sysctl states.
struct SysctlDiff {
    struct Entry {
        std::string key;
        std::string current;
        std::string target;
    };
    std::vector<Entry> changed;
    std::vector<Entry> added;
    std::vector<Entry> removed;
};

class SysctlManager {
public:
    explicit SysctlManager(const std::filesystem::path& profile_dir = "/etc/straylight/sysctl.d");

    /// Read a sysctl value from /proc/sys.
    Result<std::string, std::string> get(const std::string& key) const;

    /// Write a sysctl value to /proc/sys.
    Result<void, std::string> set(const std::string& key, const std::string& value);

    /// List all available profiles.
    std::vector<std::string> list_profiles() const;

    /// Load a profile by name.
    Result<SysctlProfile, std::string> load_profile(const std::string& name) const;

    /// Save current sysctl state (or subset) as a named profile.
    Result<void, std::string> save_profile(const std::string& name,
                                            const std::string& description = "",
                                            const std::vector<std::string>& keys = {});

    /// Apply a profile — writes all its params to /proc/sys.
    Result<void, std::string> apply(const std::string& profile_name);

    /// Diff current system state against a profile.
    Result<SysctlDiff, std::string> diff(const std::string& profile_name) const;

    /// Diff current state against default (i.e., the state saved before last apply).
    Result<SysctlDiff, std::string> diff_default() const;

    /// Rollback to the state saved before the last apply.
    Result<void, std::string> rollback();

    /// Read all current sysctl values that match a prefix.
    std::map<std::string, std::string> read_all(const std::string& prefix = "") const;

private:
    /// Convert sysctl key (dot notation) to /proc/sys path.
    std::filesystem::path key_to_proc_path(const std::string& key) const;

    /// Convert /proc/sys path to sysctl key.
    std::string proc_path_to_key(const std::filesystem::path& path) const;

    /// Read file content, trimmed.
    std::string read_file(const std::filesystem::path& path) const;

    /// Write file content.
    bool write_file(const std::filesystem::path& path, const std::string& content) const;

    std::filesystem::path profile_dir_;
    static constexpr const char* ROLLBACK_FILE = "/tmp/straylight-sysctl-rollback.json";
};

} // namespace straylight
