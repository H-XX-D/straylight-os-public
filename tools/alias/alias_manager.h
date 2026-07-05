// tools/alias/alias_manager.h
// Command alias manager — persistent shell aliases with categories.
#pragma once

#include <straylight/result.h>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace straylight {

struct AliasEntry {
    std::string name;
    std::string command;
    std::string category;
    std::string description;
};

class AliasManager {
public:
    explicit AliasManager(const std::filesystem::path& config_path = "");

    Result<void, std::string> load();
    Result<void, std::string> save() const;
    Result<void, std::string> add(const std::string& name, const std::string& command,
                                   const std::string& category = "custom",
                                   const std::string& description = "");
    Result<void, std::string> remove(const std::string& name);
    std::vector<AliasEntry> list(const std::string& category = "") const;
    std::vector<AliasEntry> search(const std::string& query) const;
    std::vector<std::string> categories() const;
    Result<int, std::string> import_from_shell(const std::filesystem::path& rc_path);
    std::string export_shell() const;
    Result<void, std::string> generate_rc_include(const std::filesystem::path& output_path) const;
    Result<void, std::string> sync();

private:
    std::filesystem::path config_path_;
    std::map<std::string, AliasEntry> aliases_;
};

} // namespace straylight
