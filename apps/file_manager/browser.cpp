// apps/file_manager/browser.cpp
// Directory browser implementation using std::filesystem
#include "browser.h"

#include <algorithm>
#include <ctime>
#include <iomanip>
#include <sstream>

#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

namespace straylight::file_manager {

std::string FileEntry::size_string() const {
    if (is_directory) return "--";

    constexpr double KB = 1024.0;
    constexpr double MB = KB * 1024.0;
    constexpr double GB = MB * 1024.0;
    constexpr double TB = GB * 1024.0;

    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1);

    double s = static_cast<double>(size);
    if (s < KB) {
        ss << size << " B";
    } else if (s < MB) {
        ss << (s / KB) << " KB";
    } else if (s < GB) {
        ss << (s / MB) << " MB";
    } else if (s < TB) {
        ss << (s / GB) << " GB";
    } else {
        ss << (s / TB) << " TB";
    }
    return ss.str();
}

std::string FileEntry::time_string() const {
    // Convert file_time_type to system clock
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        modified_time - fs::file_time_type::clock::now() +
        std::chrono::system_clock::now());
    auto time_t_val = std::chrono::system_clock::to_time_t(sctp);

    struct tm tm_buf {};
    localtime_r(&time_t_val, &tm_buf);

    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm_buf);
    return buf;
}

std::string FileEntry::perms_string() const {
    std::string s(9, '-');
    auto p = permissions;
    if ((p & fs::perms::owner_read) != fs::perms::none)    s[0] = 'r';
    if ((p & fs::perms::owner_write) != fs::perms::none)   s[1] = 'w';
    if ((p & fs::perms::owner_exec) != fs::perms::none)    s[2] = 'x';
    if ((p & fs::perms::group_read) != fs::perms::none)    s[3] = 'r';
    if ((p & fs::perms::group_write) != fs::perms::none)   s[4] = 'w';
    if ((p & fs::perms::group_exec) != fs::perms::none)    s[5] = 'x';
    if ((p & fs::perms::others_read) != fs::perms::none)   s[6] = 'r';
    if ((p & fs::perms::others_write) != fs::perms::none)  s[7] = 'w';
    if ((p & fs::perms::others_exec) != fs::perms::none)   s[8] = 'x';
    return s;
}

Browser::Browser() {
    // Start at home directory
    const char* home = getenv("HOME");
    if (home && home[0] != '\0') {
        current_path_ = home;
    } else {
        current_path_ = "/";
    }
}

Result<void, std::string> Browser::navigate(const fs::path& path) {
    std::error_code ec;
    auto canonical = fs::canonical(path, ec);
    if (ec) {
        // Try the path as-is if canonical fails
        canonical = path;
    }

    if (!fs::is_directory(canonical, ec)) {
        return Result<void, std::string>::error(
            "Not a directory: " + canonical.string());
    }

    current_path_ = canonical;

    // Update history
    if (history_pos_ >= 0 &&
        history_pos_ < static_cast<int>(history_.size()) &&
        history_[static_cast<size_t>(history_pos_)] == current_path_) {
        // Already at this position, don't duplicate
    } else {
        // Truncate forward history
        if (history_pos_ + 1 < static_cast<int>(history_.size())) {
            history_.resize(static_cast<size_t>(history_pos_ + 1));
        }
        history_.push_back(current_path_);
        history_pos_ = static_cast<int>(history_.size()) - 1;
    }

    scan_directory();
    apply_sort_and_filter();
    selected_ = -1;

    return Result<void, std::string>::ok();
}

Result<void, std::string> Browser::navigate_up() {
    auto parent = current_path_.parent_path();
    if (parent == current_path_) {
        return Result<void, std::string>::error("Already at root");
    }
    return navigate(parent);
}

Result<void, std::string> Browser::navigate_back() {
    if (!can_go_back()) {
        return Result<void, std::string>::error("No back history");
    }
    history_pos_--;
    current_path_ = history_[static_cast<size_t>(history_pos_)];
    scan_directory();
    apply_sort_and_filter();
    selected_ = -1;
    return Result<void, std::string>::ok();
}

Result<void, std::string> Browser::navigate_forward() {
    if (!can_go_forward()) {
        return Result<void, std::string>::error("No forward history");
    }
    history_pos_++;
    current_path_ = history_[static_cast<size_t>(history_pos_)];
    scan_directory();
    apply_sort_and_filter();
    selected_ = -1;
    return Result<void, std::string>::ok();
}

Result<void, std::string> Browser::refresh() {
    scan_directory();
    apply_sort_and_filter();
    return Result<void, std::string>::ok();
}

std::vector<std::pair<std::string, fs::path>> Browser::breadcrumbs() const {
    std::vector<std::pair<std::string, fs::path>> crumbs;

    fs::path accumulated;
    for (const auto& component : current_path_) {
        accumulated /= component;
        std::string name = component.string();
        if (name == "/") name = "/";
        crumbs.emplace_back(name, accumulated);
    }

    return crumbs;
}

void Browser::set_sort(SortBy by, SortDir dir) {
    sort_by_ = by;
    sort_dir_ = dir;
    apply_sort_and_filter();
}

void Browser::set_filter(const std::string& f) {
    filter_ = f;
    apply_sort_and_filter();
}

void Browser::set_show_hidden(bool show) {
    show_hidden_ = show;
    apply_sort_and_filter();
}

const FileEntry* Browser::selected_entry() const {
    if (selected_ < 0 || selected_ >= static_cast<int>(filtered_entries_.size())) {
        return nullptr;
    }
    return &filtered_entries_[static_cast<size_t>(selected_)];
}

void Browser::scan_directory() {
    all_entries_.clear();

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(current_path_, ec)) {
        FileEntry fe;
        fe.full_path = entry.path();
        fe.name = entry.path().filename().string();
        fe.is_hidden = (!fe.name.empty() && fe.name[0] == '.');
        fe.is_symlink = entry.is_symlink(ec);

        auto status = entry.symlink_status(ec);
        if (ec) continue;

        if (fe.is_symlink) {
            // Resolve symlink for type detection
            auto resolved_status = entry.status(ec);
            if (!ec) {
                fe.is_directory = fs::is_directory(resolved_status);
            }
        } else {
            fe.is_directory = fs::is_directory(status);
        }

        fe.permissions = status.permissions();

        if (!fe.is_directory) {
            fe.size = entry.file_size(ec);
            if (ec) fe.size = 0;
            fe.extension = entry.path().extension().string();
        }

        fe.modified_time = entry.last_write_time(ec);

        // Get owner
        struct stat st {};
        if (stat(fe.full_path.c_str(), &st) == 0) {
            struct passwd* pw = getpwuid(st.st_uid);
            if (pw) {
                fe.owner = pw->pw_name;
            }
        }

        all_entries_.push_back(std::move(fe));
    }
}

void Browser::apply_sort_and_filter() {
    filtered_entries_.clear();

    // Filter
    for (const auto& entry : all_entries_) {
        // Hidden filter
        if (entry.is_hidden && !show_hidden_) continue;

        // Extension/name filter
        if (!filter_.empty()) {
            // Case-insensitive substring match on name
            std::string lower_name = entry.name;
            std::string lower_filter = filter_;
            std::transform(lower_name.begin(), lower_name.end(),
                          lower_name.begin(), ::tolower);
            std::transform(lower_filter.begin(), lower_filter.end(),
                          lower_filter.begin(), ::tolower);

            if (lower_name.find(lower_filter) == std::string::npos) {
                continue;
            }
        }

        filtered_entries_.push_back(entry);
    }

    // Sort — directories first, then by chosen criteria
    auto comparator = [this](const FileEntry& a, const FileEntry& b) -> bool {
        // Directories always come first
        if (a.is_directory != b.is_directory) {
            return a.is_directory;
        }

        bool less = false;
        switch (sort_by_) {
        case SortBy::Name: {
            std::string an = a.name, bn = b.name;
            std::transform(an.begin(), an.end(), an.begin(), ::tolower);
            std::transform(bn.begin(), bn.end(), bn.begin(), ::tolower);
            less = an < bn;
            break;
        }
        case SortBy::Size:
            less = a.size < b.size;
            break;
        case SortBy::DateModified:
            less = a.modified_time < b.modified_time;
            break;
        case SortBy::Extension: {
            std::string ae = a.extension, be = b.extension;
            std::transform(ae.begin(), ae.end(), ae.begin(), ::tolower);
            std::transform(be.begin(), be.end(), be.begin(), ::tolower);
            less = ae < be;
            break;
        }
        }

        return sort_dir_ == SortDir::Ascending ? less : !less;
    };

    std::sort(filtered_entries_.begin(), filtered_entries_.end(), comparator);
}

} // namespace straylight::file_manager
