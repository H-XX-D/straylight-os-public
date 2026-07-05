// tools/alias/alias_manager.cpp
#include "alias_manager.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

namespace straylight {

namespace fs = std::filesystem;

AliasManager::AliasManager(const fs::path& config_path) {
    if (config_path.empty()) {
        const char* home = std::getenv("HOME");
        if (home) {
            config_path_ = fs::path(home) / ".config" / "straylight" / "aliases.json";
        } else {
            config_path_ = "/etc/straylight/aliases.json";
        }
    } else {
        config_path_ = config_path;
    }
}

Result<void, std::string> AliasManager::load() {
    if (!fs::exists(config_path_)) return Result<void, std::string>::ok();
    std::ifstream ifs(config_path_);
    if (!ifs) return Result<void, std::string>::error("Cannot read: " + config_path_.string());
    try {
        nlohmann::json j;
        ifs >> j;
        aliases_.clear();
        if (j.contains("aliases") && j["aliases"].is_array()) {
            for (const auto& aj : j["aliases"]) {
                AliasEntry e;
                e.name = aj.value("name", "");
                e.command = aj.value("command", "");
                e.category = aj.value("category", "custom");
                e.description = aj.value("description", "");
                if (!e.name.empty()) aliases_[e.name] = std::move(e);
            }
        }
        return Result<void, std::string>::ok();
    } catch (const std::exception& ex) {
        return Result<void, std::string>::error(std::string("Parse error: ") + ex.what());
    }
}

Result<void, std::string> AliasManager::save() const {
    std::error_code ec;
    fs::create_directories(config_path_.parent_path(), ec);
    nlohmann::json j;
    j["aliases"] = nlohmann::json::array();
    for (const auto& [_, e] : aliases_) {
        j["aliases"].push_back({{"name", e.name}, {"command", e.command},
                                {"category", e.category}, {"description", e.description}});
    }
    std::ofstream ofs(config_path_);
    if (!ofs) return Result<void, std::string>::error("Cannot write: " + config_path_.string());
    ofs << j.dump(2) << "\n";
    return Result<void, std::string>::ok();
}

Result<void, std::string> AliasManager::add(const std::string& name, const std::string& command,
                                              const std::string& category,
                                              const std::string& description) {
    if (name.empty()) return Result<void, std::string>::error("Name cannot be empty");
    if (command.empty()) return Result<void, std::string>::error("Command cannot be empty");
    aliases_[name] = {name, command, category, description};
    return save();
}

Result<void, std::string> AliasManager::remove(const std::string& name) {
    auto it = aliases_.find(name);
    if (it == aliases_.end()) return Result<void, std::string>::error("Not found: " + name);
    aliases_.erase(it);
    return save();
}

std::vector<AliasEntry> AliasManager::list(const std::string& category) const {
    std::vector<AliasEntry> result;
    for (const auto& [_, e] : aliases_) {
        if (category.empty() || e.category == category) result.push_back(e);
    }
    std::sort(result.begin(), result.end(), [](const AliasEntry& a, const AliasEntry& b) {
        return a.category != b.category ? a.category < b.category : a.name < b.name;
    });
    return result;
}

std::vector<AliasEntry> AliasManager::search(const std::string& query) const {
    std::vector<AliasEntry> result;
    std::string lq = query;
    std::transform(lq.begin(), lq.end(), lq.begin(), ::tolower);
    for (const auto& [_, e] : aliases_) {
        std::string ln = e.name, lc = e.command;
        std::transform(ln.begin(), ln.end(), ln.begin(), ::tolower);
        std::transform(lc.begin(), lc.end(), lc.begin(), ::tolower);
        if (ln.find(lq) != std::string::npos || lc.find(lq) != std::string::npos) {
            result.push_back(e);
        }
    }
    return result;
}

std::vector<std::string> AliasManager::categories() const {
    std::set<std::string> cats;
    for (const auto& [_, e] : aliases_) cats.insert(e.category);
    return {cats.begin(), cats.end()};
}

Result<int, std::string> AliasManager::import_from_shell(const fs::path& rc_path) {
    std::ifstream ifs(rc_path);
    if (!ifs) return Result<int, std::string>::error("Cannot read: " + rc_path.string());
    std::regex alias_re(R"(^\s*alias\s+(\S+)=(.+)\s*$)");
    int imported = 0;
    std::string line;
    while (std::getline(ifs, line)) {
        std::smatch match;
        if (std::regex_match(line, match, alias_re)) {
            std::string name = match[1].str();
            std::string cmd = match[2].str();
            if ((cmd.front() == '\'' && cmd.back() == '\'') ||
                (cmd.front() == '"' && cmd.back() == '"'))
                cmd = cmd.substr(1, cmd.size() - 2);
            if (!aliases_.count(name)) {
                aliases_[name] = {name, cmd, "imported", ""};
                imported++;
            }
        }
    }
    if (imported > 0) save();
    return Result<int, std::string>::ok(imported);
}

std::string AliasManager::export_shell() const {
    std::ostringstream out;
    out << "# StrayLight aliases\n";
    std::string cat;
    for (const auto& e : list()) {
        if (e.category != cat) { cat = e.category; out << "# --- " << cat << " ---\n"; }
        if (!e.description.empty()) out << "# " << e.description << "\n";
        out << "alias " << e.name << "='" << e.command << "'\n";
    }
    return out.str();
}

Result<void, std::string> AliasManager::generate_rc_include(const fs::path& output_path) const {
    std::error_code ec;
    fs::create_directories(output_path.parent_path(), ec);
    std::ofstream ofs(output_path);
    if (!ofs) return Result<void, std::string>::error("Cannot write: " + output_path.string());
    ofs << export_shell();
    return Result<void, std::string>::ok();
}

Result<void, std::string> AliasManager::sync() { return load(); }

} // namespace straylight
