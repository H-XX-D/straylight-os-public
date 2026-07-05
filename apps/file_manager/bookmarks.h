// apps/file_manager/bookmarks.h
// Sidebar bookmarks — built-in and custom, persisted to JSON
#pragma once

#include <straylight/result.h>

#include <filesystem>
#include <string>
#include <vector>

namespace straylight::file_manager {

namespace fs = std::filesystem;

/// A single bookmark entry.
struct Bookmark {
    std::string name;
    fs::path path;
    std::string icon; // icon identifier (e.g., "home", "documents", "folder")
    bool builtin = false; // built-in bookmarks can't be removed

    bool operator==(const Bookmark& o) const {
        return name == o.name && path == o.path;
    }
};

/// Bookmark manager — maintains built-in and user-defined bookmarks.
class Bookmarks {
public:
    Bookmarks();

    /// Initialize with built-in bookmarks.
    void init();

    /// Load custom bookmarks from JSON file.
    Result<void, std::string> load(const fs::path& config_path);

    /// Save custom bookmarks to JSON file.
    Result<void, std::string> save(const fs::path& config_path) const;

    /// Load from default path, or just use built-ins.
    void load_or_defaults();

    /// Get all bookmarks (built-in + custom).
    [[nodiscard]] const std::vector<Bookmark>& all() const { return bookmarks_; }

    /// Add a custom bookmark.
    Result<void, std::string> add(const std::string& name, const fs::path& path);

    /// Remove a bookmark by index.
    Result<void, std::string> remove(size_t index);

    /// Move a bookmark up or down.
    void move_up(size_t index);
    void move_down(size_t index);

    /// Render the bookmarks sidebar in ImGui.
    /// Returns the path of the clicked bookmark, or empty if none clicked.
    fs::path render();

private:
    std::vector<Bookmark> bookmarks_;
    fs::path config_path_;
};

} // namespace straylight::file_manager
