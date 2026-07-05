// tools/keybind/keybind_manager.cpp
#include "keybind_manager.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace straylight {

namespace fs = std::filesystem;

KeybindManager::KeybindManager(const fs::path& config_path) {
    if (config_path.empty()) {
        const char* home = std::getenv("HOME");
        config_path_ = home ? fs::path(home) / ".config" / "straylight" / "keybinds.json"
                            : fs::path("/etc/straylight/keybinds.json");
    } else {
        config_path_ = config_path;
    }
    compositor_config_ = "/etc/straylight/compositor.d/keybinds.conf";
    load_defaults();
}

void KeybindManager::load_defaults() {
    auto d = [&](const std::string& a, const std::string& k, const std::string& c, const std::string& desc) {
        Keybinding kb{a, normalize_keys(k), c, desc, true};
        defaults_[a] = kb;
    };
    d("terminal","Super+Return","system","Open terminal");
    d("launcher","Super+D","system","Open launcher");
    d("lock-screen","Super+L","system","Lock screen");
    d("logout","Super+Shift+E","system","Logout");
    d("screenshot","Print","system","Screenshot");
    d("screenshot-area","Shift+Print","system","Area screenshot");
    d("close-window","Alt+F4","system","Close window");
    d("kill-window","Super+Shift+Q","system","Kill window");
    d("toggle-fullscreen","Super+F","system","Toggle fullscreen");
    d("toggle-float","Super+Shift+Space","system","Toggle float");
    d("workspace-1","Super+1","workspace","Workspace 1");
    d("workspace-2","Super+2","workspace","Workspace 2");
    d("workspace-3","Super+3","workspace","Workspace 3");
    d("workspace-4","Super+4","workspace","Workspace 4");
    d("move-to-ws-1","Super+Shift+1","workspace","Move to ws 1");
    d("move-to-ws-2","Super+Shift+2","workspace","Move to ws 2");
    d("move-to-ws-3","Super+Shift+3","workspace","Move to ws 3");
    d("move-to-ws-4","Super+Shift+4","workspace","Move to ws 4");
    d("volume-up","XF86AudioRaiseVolume","media","Volume up");
    d("volume-down","XF86AudioLowerVolume","media","Volume down");
    d("volume-mute","XF86AudioMute","media","Mute");
    d("media-play","XF86AudioPlay","media","Play/pause");
    d("brightness-up","XF86MonBrightnessUp","media","Brightness up");
    d("brightness-down","XF86MonBrightnessDown","media","Brightness down");
    d("files","Super+E","app-launch","File manager");
    d("browser","Super+B","app-launch","Browser");
    d("settings","Super+I","app-launch","Settings");
}

std::string KeybindManager::normalize_keys(const std::string& keys) const {
    std::vector<std::string> parts;
    std::istringstream iss(keys); std::string part;
    while (std::getline(iss, part, '+')) {
        while (!part.empty() && part.front() == ' ') part.erase(part.begin());
        while (!part.empty() && part.back() == ' ') part.pop_back();
        if (!part.empty()) parts.push_back(part);
    }
    if (parts.empty()) return keys;
    std::set<std::string> mods_set{"Super","Ctrl","Alt","Shift","Meta"};
    std::vector<std::string> mods; std::string key;
    for (auto& p : parts) {
        if (p == "super" || p == "SUPER" || p == "Mod4") p = "Super";
        if (p == "ctrl" || p == "CTRL" || p == "Control") p = "Ctrl";
        if (p == "alt" || p == "ALT" || p == "Mod1") p = "Alt";
        if (p == "shift" || p == "SHIFT") p = "Shift";
        if (mods_set.count(p)) mods.push_back(p); else key = p;
    }
    std::sort(mods.begin(), mods.end());
    std::string r;
    for (const auto& m : mods) { if (!r.empty()) r += "+"; r += m; }
    if (!key.empty()) { if (!r.empty()) r += "+"; r += key; }
    return r;
}

Result<void, std::string> KeybindManager::load() {
    bindings_ = defaults_;
    if (!fs::exists(config_path_)) return Result<void, std::string>::ok();
    std::ifstream ifs(config_path_);
    if (!ifs) return Result<void, std::string>::error("Cannot read: " + config_path_.string());
    try {
        nlohmann::json j; ifs >> j;
        if (j.contains("bindings") && j["bindings"].is_array()) {
            for (const auto& bj : j["bindings"]) {
                Keybinding kb;
                kb.action = bj.value("action", "");
                kb.keys = normalize_keys(bj.value("keys", ""));
                kb.category = bj.value("category", "custom");
                kb.description = bj.value("description", "");
                kb.is_default = false;
                if (!kb.action.empty()) bindings_[kb.action] = std::move(kb);
            }
        }
        return Result<void, std::string>::ok();
    } catch (const std::exception& e) {
        return Result<void, std::string>::error(std::string("Parse error: ") + e.what());
    }
}

Result<void, std::string> KeybindManager::save() const {
    std::error_code ec; fs::create_directories(config_path_.parent_path(), ec);
    nlohmann::json j; j["bindings"] = nlohmann::json::array();
    for (const auto& [_, kb] : bindings_) {
        auto def = defaults_.find(kb.action);
        if (def != defaults_.end() && def->second.keys == kb.keys) continue;
        j["bindings"].push_back({{"action",kb.action},{"keys",kb.keys},
                                  {"category",kb.category},{"description",kb.description}});
    }
    std::ofstream ofs(config_path_);
    if (!ofs) return Result<void, std::string>::error("Cannot write");
    ofs << j.dump(2) << "\n";
    return Result<void, std::string>::ok();
}

std::vector<Keybinding> KeybindManager::list(const std::string& category) const {
    std::vector<Keybinding> r;
    for (const auto& [_, kb] : bindings_)
        if (category.empty() || kb.category == category) r.push_back(kb);
    std::sort(r.begin(), r.end(), [](const Keybinding& a, const Keybinding& b) {
        return a.category != b.category ? a.category < b.category : a.action < b.action;
    });
    return r;
}

Result<void, std::string> KeybindManager::set(const std::string& action, const std::string& keys,
                                                const std::string& category, const std::string& description) {
    std::string norm = normalize_keys(keys);
    Keybinding kb; kb.action = action; kb.keys = norm; kb.is_default = false;
    auto ex = bindings_.find(action);
    kb.category = category.empty() ? (ex != bindings_.end() ? ex->second.category : "custom") : category;
    kb.description = description.empty() ? (ex != bindings_.end() ? ex->second.description : "") : description;
    bindings_[action] = std::move(kb);
    return save();
}

Result<void, std::string> KeybindManager::remove(const std::string& action) {
    if (!bindings_.count(action)) return Result<void, std::string>::error("Not found: " + action);
    bindings_.erase(action);
    return save();
}

std::vector<KeyConflict> KeybindManager::find_conflicts() const {
    std::vector<KeyConflict> conflicts;
    std::map<std::string, std::string> seen;
    for (const auto& [action, kb] : bindings_) {
        auto it = seen.find(kb.keys);
        if (it != seen.end() && it->second != action)
            conflicts.push_back({kb.keys, it->second, action});
        else seen[kb.keys] = action;
    }
    return conflicts;
}

Result<void, std::string> KeybindManager::reset(const std::string& action) {
    if (action.empty()) bindings_ = defaults_;
    else {
        auto d = defaults_.find(action);
        if (d != defaults_.end()) bindings_[action] = d->second;
        else bindings_.erase(action);
    }
    return save();
}

std::vector<std::string> KeybindManager::categories() const {
    std::set<std::string> cats;
    for (const auto& [_, kb] : bindings_) cats.insert(kb.category);
    return {cats.begin(), cats.end()};
}

std::string KeybindManager::export_json() const {
    nlohmann::json j; j["bindings"] = nlohmann::json::array();
    for (const auto& [_, kb] : bindings_)
        j["bindings"].push_back({{"action",kb.action},{"keys",kb.keys},
                                  {"category",kb.category},{"description",kb.description}});
    return j.dump(2);
}

Result<int, std::string> KeybindManager::import_json(const std::string& json_str) {
    try {
        auto j = nlohmann::json::parse(json_str);
        int n = 0;
        if (j.contains("bindings") && j["bindings"].is_array()) {
            for (const auto& bj : j["bindings"]) {
                Keybinding kb;
                kb.action = bj.value("action", "");
                kb.keys = normalize_keys(bj.value("keys", ""));
                kb.category = bj.value("category", "custom");
                kb.description = bj.value("description", "");
                if (!kb.action.empty()) { bindings_[kb.action] = std::move(kb); n++; }
            }
        }
        save();
        return Result<int, std::string>::ok(n);
    } catch (const std::exception& e) {
        return Result<int, std::string>::error(std::string("Parse error: ") + e.what());
    }
}

Result<void, std::string> KeybindManager::apply_to_compositor() const {
    std::error_code ec; fs::create_directories(compositor_config_.parent_path(), ec);
    std::ofstream ofs(compositor_config_);
    if (!ofs) return Result<void, std::string>::error("Cannot write: " + compositor_config_.string());
    ofs << "# StrayLight compositor keybindings (auto-generated)\n\n";
    std::string cat;
    for (const auto& kb : list()) {
        if (kb.category != cat) { cat = kb.category; ofs << "# --- " << cat << " ---\n"; }
        if (!kb.description.empty()) ofs << "# " << kb.description << "\n";
        ofs << "bind = " << kb.keys << ", " << kb.action << "\n";
    }
    return Result<void, std::string>::ok();
}

} // namespace straylight
