// apps/file_manager/browser.h
// Directory browser with sorting, filtering, and navigation
#pragma once

#include <straylight/result.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace straylight::file_manager {

namespace fs = std::filesystem;

/// Sort criteria for file listings.
enum class SortBy {
    Name,
    Size,
    DateModified,
    Extension,
};

/// Sort direction.
enum class SortDir {
    Ascending,
    Descending,
};

/// A single directory entry with metadata.
struct FileEntry {
    std::string name;
    fs::path full_path;
    bool is_directory = false;
    bool is_symlink = false;
    bool is_hidden = false;
    uintmax_t size = 0;
    fs::file_time_type modified_time{};
    fs::perms permissions{};
    std::string extension;
    std::string owner;

    /// Human-readable size string.
    [[nodiscard]] std::string size_string() const;

    /// Human-readable modified time string.
    [[nodiscard]] std::string time_string() const;

    /// Permission string like "rwxr-xr-x".
    [[nodiscard]] std::string perms_string() const;
};

/// Directory browser — maintains the current directory listing and navigation
/// history.
class Browser {
public:
    Browser();

    /// Navigate to a directory.
    Result<void, std::string> navigate(const fs::path& path);

    /// Navigate up to parent directory.
    Result<void, std::string> navigate_up();

    /// Navigate back in history.
    Result<void, std::string> navigate_back();

    /// Navigate forward in history.
    Result<void, std::string> navigate_forward();

    /// Refresh the current directory listing.
    Result<void, std::string> refresh();

    /// Get the current directory.
    [[nodiscard]] const fs::path& current_path() const { return current_path_; }

    /// Get the current file listing (sorted and filtered).
    [[nodiscard]] const std::vector<FileEntry>& entries() const { return filtered_entries_; }

    /// Get breadcrumb path components.
    [[nodiscard]] std::vector<std::pair<std::string, fs::path>> breadcrumbs() const;

    /// Sort settings.
    void set_sort(SortBy by, SortDir dir);
    [[nodiscard]] SortBy sort_by() const { return sort_by_; }
    [[nodiscard]] SortDir sort_dir() const { return sort_dir_; }

    /// Filter settings.
    void set_filter(const std::string& filter);
    [[nodiscard]] const std::string& filter() const { return filter_; }

    /// Show hidden files.
    void set_show_hidden(bool show);
    [[nodiscard]] bool show_hidden() const { return show_hidden_; }

    /// Check history state.
    [[nodiscard]] bool can_go_back() const { return history_pos_ > 0; }
    [[nodiscard]] bool can_go_forward() const {
        return history_pos_ + 1 < static_cast<int>(history_.size());
    }

    /// Get selected entry index (-1 if none).
    [[nodiscard]] int selected_index() const { return selected_; }
    void set_selected(int idx) { selected_ = idx; }

    /// Get the selected entry, if any.
    [[nodiscard]] const FileEntry* selected_entry() const;

private:
    void apply_sort_and_filter();
    void scan_directory();

    fs::path current_path_;
    std::vector<FileEntry> all_entries_;
    std::vector<FileEntry> filtered_entries_;

    // History
    std::vector<fs::path> history_;
    int history_pos_ = -1;

    // Sort/filter
    SortBy sort_by_ = SortBy::Name;
    SortDir sort_dir_ = SortDir::Ascending;
    std::string filter_;
    bool show_hidden_ = false;

    int selected_ = -1;
};

} // namespace straylight::file_manager
