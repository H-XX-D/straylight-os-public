// tools/keybind/keybind_manager.h
// Keyboard shortcut manager — compositor keybindings with conflict detection.
#pragma once

#include <straylight/result.h>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace straylight {

struct Keybinding {
    std::string action;
    std::string keys;
    std::string category;
    std::string description;
    bool is_default = false;
};

struct KeyConflict {
    std::string keys;
    std::string action1;
    std::string action2;
};

class KeybindManager {
public:
    explicit KeybindManager(const std::filesystem::path& config_path = "");

    Result<void, std::string> load();
    Result<void, std::string> save() const;
    std::vector<Keybinding> list(const std::string& category = "") const;
    Result<void, std::string> set(const std::string& action, const std::string& keys,
                                   const std::string& category = "custom",
                                   const std::string& description = "");
    Result<void, std::string> remove(const std::string& action);
    std::vector<KeyConflict> find_conflicts() const;
    Result<void, std::string> reset(const std::string& action = "");
    std::vector<std::string> categories() const;
    std::string export_json() const;
    Result<int, std::string> import_json(const std::string& json_str);
    Result<void, std::string> apply_to_compositor() const;
    void load_defaults();

private:
    std::string normalize_keys(const std::string& keys) const;
    std::filesystem::path config_path_;
    std::filesystem::path compositor_config_;
    std::map<std::string, Keybinding> bindings_;
    std::map<std::string, Keybinding> defaults_;
};

} // namespace straylight
