// apps/file_manager/bookmarks.cpp
// Bookmark management with JSON persistence
#include "bookmarks.h"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>

namespace straylight::file_manager {

using json = nlohmann::json;

Bookmarks::Bookmarks() = default;

void Bookmarks::init() {
    bookmarks_.clear();

    const char* home = getenv("HOME");
    std::string home_dir = home ? home : "/root";

    bookmarks_.push_back({
        "Home", fs::path(home_dir), "home", true
    });
    bookmarks_.push_back({
        "Desktop", fs::path(home_dir) / "Desktop", "desktop", true
    });
    bookmarks_.push_back({
        "Documents", fs::path(home_dir) / "Documents", "documents", true
    });
    bookmarks_.push_back({
        "Downloads", fs::path(home_dir) / "Downloads", "downloads", true
    });
    bookmarks_.push_back({
        "Music", fs::path(home_dir) / "Music", "music", true
    });
    bookmarks_.push_back({
        "Pictures", fs::path(home_dir) / "Pictures", "pictures", true
    });
    bookmarks_.push_back({
        "Videos", fs::path(home_dir) / "Videos", "videos", true
    });
    bookmarks_.push_back({
        "/", fs::path("/"), "filesystem", true
    });
}

Result<void, std::string> Bookmarks::load(const fs::path& config_path) {
    config_path_ = config_path;

    std::ifstream file(config_path);
    if (!file.is_open()) {
        return Result<void, std::string>::error(
            "Cannot open bookmarks file: " + config_path.string());
    }

    json j;
    try {
        file >> j;
    } catch (const json::parse_error& e) {
        return Result<void, std::string>::error(
            std::string("JSON parse error: ") + e.what());
    }

    if (j.contains("bookmarks") && j["bookmarks"].is_array()) {
        for (const auto& entry : j["bookmarks"]) {
            if (!entry.contains("name") || !entry.contains("path")) continue;

            Bookmark bm;
            bm.name = entry["name"].get<std::string>();
            bm.path = fs::path(entry["path"].get<std::string>());
            bm.icon = entry.value("icon", "folder");
            bm.builtin = false;

            // Don't add duplicates
            bool duplicate = false;
            for (const auto& existing : bookmarks_) {
                if (existing.path == bm.path) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                bookmarks_.push_back(std::move(bm));
            }
        }
    }

    return Result<void, std::string>::ok();
}

Result<void, std::string> Bookmarks::save(const fs::path& config_path) const {
    json j;
    json arr = json::array();

    for (const auto& bm : bookmarks_) {
        if (bm.builtin) continue; // Don't persist built-in bookmarks

        json entry;
        entry["name"] = bm.name;
        entry["path"] = bm.path.string();
        entry["icon"] = bm.icon;
        arr.push_back(entry);
    }

    j["bookmarks"] = arr;

    // Ensure parent directory exists
    std::error_code ec;
    if (config_path.has_parent_path()) {
        fs::create_directories(config_path.parent_path(), ec);
    }

    std::ofstream file(config_path);
    if (!file.is_open()) {
        return Result<void, std::string>::error(
            "Cannot write bookmarks file: " + config_path.string());
    }

    file << j.dump(2);

    return Result<void, std::string>::ok();
}

void Bookmarks::load_or_defaults() {
    init();

    // Try loading custom bookmarks
    std::vector<fs::path> paths;

    const char* xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0] != '\0') {
        paths.emplace_back(std::string(xdg) + "/straylight/bookmarks.json");
    }

    const char* home = getenv("HOME");
    if (home && home[0] != '\0') {
        paths.emplace_back(
            std::string(home) + "/.config/straylight/bookmarks.json");
    }

    for (const auto& path : paths) {
        if (fs::exists(path)) {
            config_path_ = path;
            load(path);
            return;
        }
    }

    // Set default config path for saving
    if (home && home[0] != '\0') {
        config_path_ = std::string(home) + "/.config/straylight/bookmarks.json";
    }
}

Result<void, std::string> Bookmarks::add(const std::string& name,
                                           const fs::path& path) {
    // Check for duplicates
    for (const auto& bm : bookmarks_) {
        if (bm.path == path) {
            return Result<void, std::string>::error(
                "Bookmark already exists for: " + path.string());
        }
    }

    bookmarks_.push_back({name, path, "folder", false});

    // Auto-save
    if (!config_path_.empty()) {
        save(config_path_);
    }

    return Result<void, std::string>::ok();
}

Result<void, std::string> Bookmarks::remove(size_t index) {
    if (index >= bookmarks_.size()) {
        return Result<void, std::string>::error("Invalid bookmark index");
    }
    if (bookmarks_[index].builtin) {
        return Result<void, std::string>::error("Cannot remove built-in bookmark");
    }

    bookmarks_.erase(bookmarks_.begin() + static_cast<ptrdiff_t>(index));

    if (!config_path_.empty()) {
        save(config_path_);
    }

    return Result<void, std::string>::ok();
}

void Bookmarks::move_up(size_t index) {
    if (index <= 0 || index >= bookmarks_.size()) return;
    if (bookmarks_[index].builtin || bookmarks_[index - 1].builtin) return;
    std::swap(bookmarks_[index], bookmarks_[index - 1]);
}

void Bookmarks::move_down(size_t index) {
    if (index + 1 >= bookmarks_.size()) return;
    if (bookmarks_[index].builtin || bookmarks_[index + 1].builtin) return;
    std::swap(bookmarks_[index], bookmarks_[index + 1]);
}

fs::path Bookmarks::render() {
    fs::path clicked_path;

    ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.67f, 1.0f), "Bookmarks");
    ImGui::Separator();

    for (size_t i = 0; i < bookmarks_.size(); ++i) {
        const auto& bm = bookmarks_[i];

        // Icon prefix based on type
        const char* icon_prefix = "";
        if (bm.icon == "home") icon_prefix = "[~] ";
        else if (bm.icon == "desktop") icon_prefix = "[D] ";
        else if (bm.icon == "documents") icon_prefix = "[d] ";
        else if (bm.icon == "downloads") icon_prefix = "[v] ";
        else if (bm.icon == "music") icon_prefix = "[m] ";
        else if (bm.icon == "pictures") icon_prefix = "[p] ";
        else if (bm.icon == "videos") icon_prefix = "[V] ";
        else if (bm.icon == "filesystem") icon_prefix = "[/] ";
        else icon_prefix = "[*] ";

        std::string label = std::string(icon_prefix) + bm.name;

        if (ImGui::Selectable(label.c_str())) {
            clicked_path = bm.path;
        }

        // Right-click context menu for custom bookmarks
        if (!bm.builtin && ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Remove Bookmark")) {
                remove(i);
                ImGui::EndPopup();
                break; // Iterator invalidated
            }
            ImGui::EndPopup();
        }
    }

    ImGui::Spacing();
    ImGui::Separator();

    // Add bookmark button
    if (ImGui::SmallButton("+ Add Bookmark")) {
        ImGui::OpenPopup("AddBookmark");
    }

    if (ImGui::BeginPopup("AddBookmark")) {
        static char name_buf[256] = {};
        static char path_buf[1024] = {};
        ImGui::InputText("Name", name_buf, sizeof(name_buf));
        ImGui::InputText("Path", path_buf, sizeof(path_buf));
        if (ImGui::Button("Add") && name_buf[0] != '\0' && path_buf[0] != '\0') {
            add(name_buf, fs::path(path_buf));
            name_buf[0] = '\0';
            path_buf[0] = '\0';
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    return clicked_path;
}

} // namespace straylight::file_manager
