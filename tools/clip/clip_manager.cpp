// tools/clip/clip_manager.cpp
// Full clipboard manager implementation for StrayLight OS.

#include "clip_manager.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

namespace straylight {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

ClipManager::ClipManager() {
    fs::create_directories(data_dir());
    fs::create_directories(images_dir());
    // Ensure history file exists
    if (!fs::exists(history_file())) {
        std::ofstream out(history_file());
        // Empty file is fine; we'll handle empty reads
    }
}

ClipManager::~ClipManager() = default;

// ---------------------------------------------------------------------------
// Path helpers
// ---------------------------------------------------------------------------

std::string ClipManager::data_dir() const {
    const char* home = std::getenv("HOME");
    if (!home) home = "/root";
    return std::string(home) + "/.local/share/straylight/clipboard";
}

std::string ClipManager::history_file() const {
    return data_dir() + "/history.dat";
}

std::string ClipManager::images_dir() const {
    return data_dir() + "/images";
}

// ---------------------------------------------------------------------------
// Shell / time helpers
// ---------------------------------------------------------------------------

Result<std::string, std::string> ClipManager::run_cmd(const std::string& cmd) const {
    std::array<char, 8192> buffer{};
    std::string output;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return Result<std::string, std::string>::error(
            "popen failed: " + std::string(strerror(errno)));
    }
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }
    int rc = pclose(pipe);
    if (rc != 0 && output.empty()) {
        return Result<std::string, std::string>::error(
            "command failed (rc=" + std::to_string(rc) + "): " + cmd);
    }
    return Result<std::string, std::string>::ok(output);
}

std::string ClipManager::now_iso() const {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    localtime_r(&time_t_now, &tm_buf);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_buf);
    return buf;
}

bool ClipManager::is_wayland() const {
    const char* session = std::getenv("WAYLAND_DISPLAY");
    return session != nullptr && session[0] != '\0';
}

// ---------------------------------------------------------------------------
// Type detection
// ---------------------------------------------------------------------------

ClipType ClipManager::detect_type(const std::string& content) const {
    if (content.empty()) return ClipType::Unknown;

    // Check for URL pattern
    std::regex url_re(R"(^https?://\S+$)", std::regex::icase);
    if (std::regex_match(content, url_re)) return ClipType::Url;

    // Check for image file path
    std::regex img_re(R"(\.(png|jpg|jpeg|gif|bmp|svg|webp)$)", std::regex::icase);
    if (std::regex_search(content, img_re)) {
        if (fs::exists(content)) return ClipType::Image;
    }

    // Check for binary/image data markers (PNG header, JPEG header)
    if (content.size() >= 4) {
        unsigned char b0 = static_cast<unsigned char>(content[0]);
        unsigned char b1 = static_cast<unsigned char>(content[1]);
        if (b0 == 0x89 && b1 == 0x50) return ClipType::Image;  // PNG
        if (b0 == 0xFF && b1 == 0xD8) return ClipType::Image;  // JPEG
    }

    return ClipType::Text;
}

std::string ClipManager::make_preview(const std::string& content, size_t max_len) const {
    std::string preview = content;
    // Replace newlines with spaces for preview
    for (auto& c : preview) {
        if (c == '\n' || c == '\r') c = ' ';
    }
    if (preview.size() > max_len) {
        preview = preview.substr(0, max_len - 3) + "...";
    }
    return preview;
}

// ---------------------------------------------------------------------------
// Persistence — simple line-based format
// Each entry: ID|TYPE|PINNED|TIMESTAMP|SIZE|MIME|CONTENT (base64 for images)
// ---------------------------------------------------------------------------

std::vector<ClipEntry> ClipManager::load_entries() const {
    std::vector<ClipEntry> entries;
    std::ifstream in(history_file());
    if (!in.is_open()) return entries;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;

        ClipEntry entry;
        std::istringstream ss(line);
        std::string field;

        // ID
        if (!std::getline(ss, field, '|')) continue;
        try { entry.id = std::stoull(field); } catch (...) { continue; }

        // Type
        if (!std::getline(ss, field, '|')) continue;
        if (field == "text") entry.type = ClipType::Text;
        else if (field == "image") entry.type = ClipType::Image;
        else if (field == "url") entry.type = ClipType::Url;
        else entry.type = ClipType::Unknown;

        // Pinned
        if (!std::getline(ss, field, '|')) continue;
        entry.pinned = (field == "1");

        // Timestamp
        if (!std::getline(ss, field, '|')) continue;
        entry.timestamp = field;

        // Size
        if (!std::getline(ss, field, '|')) continue;
        try { entry.size_bytes = std::stoull(field); } catch (...) {}

        // MIME type
        if (!std::getline(ss, field, '|')) continue;
        entry.mime_type = field;

        // Content (rest of line, may contain |)
        std::string content;
        std::getline(ss, content);
        // Unescape newlines stored as \\n
        std::string unescaped;
        for (size_t i = 0; i < content.size(); ++i) {
            if (i + 1 < content.size() && content[i] == '\\' && content[i + 1] == 'n') {
                unescaped += '\n';
                ++i;
            } else if (i + 1 < content.size() && content[i] == '\\' && content[i + 1] == '\\') {
                unescaped += '\\';
                ++i;
            } else {
                unescaped += content[i];
            }
        }
        entry.content = unescaped;
        entry.preview = make_preview(entry.content);

        entries.push_back(entry);
    }

    return entries;
}

void ClipManager::save_entries(const std::vector<ClipEntry>& entries) const {
    std::ofstream out(history_file(), std::ios::trunc);
    if (!out.is_open()) return;

    for (const auto& entry : entries) {
        std::string type_str;
        switch (entry.type) {
            case ClipType::Text: type_str = "text"; break;
            case ClipType::Image: type_str = "image"; break;
            case ClipType::Url: type_str = "url"; break;
            default: type_str = "unknown"; break;
        }

        // Escape newlines and backslashes in content
        std::string escaped;
        for (char c : entry.content) {
            if (c == '\n') escaped += "\\n";
            else if (c == '\\') escaped += "\\\\";
            else escaped += c;
        }

        out << entry.id << "|"
            << type_str << "|"
            << (entry.pinned ? "1" : "0") << "|"
            << entry.timestamp << "|"
            << entry.size_bytes << "|"
            << entry.mime_type << "|"
            << escaped << "\n";
    }
}

uint64_t ClipManager::next_id(const std::vector<ClipEntry>& entries) const {
    uint64_t max_id = 0;
    for (const auto& e : entries) {
        if (e.id > max_id) max_id = e.id;
    }
    return max_id + 1;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

Result<ClipEntry, std::string> ClipManager::paste() const {
    std::string cmd;
    if (is_wayland()) {
        cmd = "wl-paste 2>/dev/null";
    } else {
        cmd = "xclip -selection clipboard -o 2>/dev/null";
    }

    auto res = run_cmd(cmd);
    if (!res.has_value()) {
        return Result<ClipEntry, std::string>::error(
            "failed to read clipboard: " + res.error());
    }

    std::string content = res.value();
    // Remove trailing newline added by the tool
    if (!content.empty() && content.back() == '\n') {
        content.pop_back();
    }

    if (content.empty()) {
        return Result<ClipEntry, std::string>::error("clipboard is empty");
    }

    ClipEntry entry;
    entry.content = content;
    entry.type = detect_type(content);
    entry.timestamp = now_iso();
    entry.size_bytes = content.size();
    entry.preview = make_preview(content);

    switch (entry.type) {
        case ClipType::Text: entry.mime_type = "text/plain"; break;
        case ClipType::Image: entry.mime_type = "image/png"; break;
        case ClipType::Url: entry.mime_type = "text/uri-list"; break;
        default: entry.mime_type = "application/octet-stream"; break;
    }

    // Save to history
    auto entries = load_entries();
    entry.id = next_id(entries);

    // Deduplicate: remove previous identical content
    entries.erase(
        std::remove_if(entries.begin(), entries.end(),
                       [&](const ClipEntry& e) {
                           return !e.pinned && e.content == content;
                       }),
        entries.end());

    entries.insert(entries.begin(), entry);

    // Cap history at 500 non-pinned entries
    int non_pinned = 0;
    for (auto it = entries.begin(); it != entries.end(); ) {
        if (!it->pinned) {
            ++non_pinned;
            if (non_pinned > 500) {
                it = entries.erase(it);
                continue;
            }
        }
        ++it;
    }

    save_entries(entries);

    return Result<ClipEntry, std::string>::ok(entry);
}

Result<void, std::string> ClipManager::copy(const std::string& text) {
    std::string cmd;
    if (is_wayland()) {
        cmd = "echo -n '" + text + "' | wl-copy 2>/dev/null";
    } else {
        cmd = "echo -n '" + text + "' | xclip -selection clipboard 2>/dev/null";
    }

    auto res = run_cmd(cmd);
    if (!res.has_value()) {
        return Result<void, std::string>::error("failed to copy to clipboard: " + res.error());
    }

    // Add to history
    ClipEntry entry;
    entry.content = text;
    entry.type = detect_type(text);
    entry.timestamp = now_iso();
    entry.size_bytes = text.size();
    entry.preview = make_preview(text);

    switch (entry.type) {
        case ClipType::Text: entry.mime_type = "text/plain"; break;
        case ClipType::Url: entry.mime_type = "text/uri-list"; break;
        default: entry.mime_type = "text/plain"; break;
    }

    auto entries = load_entries();
    entry.id = next_id(entries);

    // Deduplicate
    entries.erase(
        std::remove_if(entries.begin(), entries.end(),
                       [&](const ClipEntry& e) {
                           return !e.pinned && e.content == text;
                       }),
        entries.end());

    entries.insert(entries.begin(), entry);
    save_entries(entries);

    return Result<void, std::string>::ok();
}

Result<std::vector<ClipEntry>, std::string> ClipManager::history(const std::string& search,
                                                                   int limit) const {
    auto entries = load_entries();

    if (!search.empty()) {
        std::string lower_search = search;
        std::transform(lower_search.begin(), lower_search.end(),
                       lower_search.begin(), ::tolower);

        std::vector<ClipEntry> filtered;
        for (const auto& e : entries) {
            std::string lower_content = e.content;
            std::transform(lower_content.begin(), lower_content.end(),
                           lower_content.begin(), ::tolower);
            if (lower_content.find(lower_search) != std::string::npos) {
                filtered.push_back(e);
            }
        }
        entries = filtered;
    }

    if (limit > 0 && static_cast<int>(entries.size()) > limit) {
        entries.resize(limit);
    }

    return Result<std::vector<ClipEntry>, std::string>::ok(entries);
}

Result<void, std::string> ClipManager::pin(uint64_t id) {
    auto entries = load_entries();
    bool found = false;
    for (auto& e : entries) {
        if (e.id == id) {
            e.pinned = true;
            found = true;
            break;
        }
    }
    if (!found) {
        return Result<void, std::string>::error("entry " + std::to_string(id) + " not found");
    }
    save_entries(entries);
    return Result<void, std::string>::ok();
}

Result<void, std::string> ClipManager::unpin(uint64_t id) {
    auto entries = load_entries();
    bool found = false;
    for (auto& e : entries) {
        if (e.id == id) {
            e.pinned = false;
            found = true;
            break;
        }
    }
    if (!found) {
        return Result<void, std::string>::error("entry " + std::to_string(id) + " not found");
    }
    save_entries(entries);
    return Result<void, std::string>::ok();
}

Result<std::vector<ClipEntry>, std::string> ClipManager::list_pinned() const {
    auto entries = load_entries();
    std::vector<ClipEntry> pinned;
    for (const auto& e : entries) {
        if (e.pinned) pinned.push_back(e);
    }
    return Result<std::vector<ClipEntry>, std::string>::ok(pinned);
}

Result<void, std::string> ClipManager::clear() {
    auto entries = load_entries();
    // Keep only pinned entries
    entries.erase(
        std::remove_if(entries.begin(), entries.end(),
                       [](const ClipEntry& e) { return !e.pinned; }),
        entries.end());
    save_entries(entries);
    return Result<void, std::string>::ok();
}

Result<void, std::string> ClipManager::remove(uint64_t id) {
    auto entries = load_entries();
    auto it = std::find_if(entries.begin(), entries.end(),
                           [id](const ClipEntry& e) { return e.id == id; });
    if (it == entries.end()) {
        return Result<void, std::string>::error("entry " + std::to_string(id) + " not found");
    }
    entries.erase(it);
    save_entries(entries);
    return Result<void, std::string>::ok();
}

Result<int, std::string> ClipManager::expire(int max_age_hours) {
    auto entries = load_entries();
    auto now = std::chrono::system_clock::now();
    int removed = 0;

    entries.erase(
        std::remove_if(entries.begin(), entries.end(),
                       [&](const ClipEntry& e) {
                           if (e.pinned) return false;

                           // Parse timestamp
                           std::tm tm_buf{};
                           std::istringstream ss(e.timestamp);
                           ss >> std::get_time(&tm_buf, "%Y-%m-%dT%H:%M:%S");
                           if (ss.fail()) return false;

                           auto entry_time = std::chrono::system_clock::from_time_t(mktime(&tm_buf));
                           auto age = std::chrono::duration_cast<std::chrono::hours>(
                               now - entry_time);

                           if (age.count() > max_age_hours) {
                               ++removed;
                               return true;
                           }
                           return false;
                       }),
        entries.end());

    save_entries(entries);
    return Result<int, std::string>::ok(removed);
}

Result<std::vector<ClipEntry>, std::string> ClipManager::list(int limit) const {
    return history("", limit);
}

} // namespace straylight
