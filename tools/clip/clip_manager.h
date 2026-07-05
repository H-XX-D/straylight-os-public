// tools/clip/clip_manager.h
// Clipboard manager for StrayLight OS — history, pinning, image support, cross-device sync.
#pragma once

#include <straylight/result.h>

#include <cstdint>
#include <string>
#include <vector>

namespace straylight {

/// Type of clipboard content.
enum class ClipType {
    Text,
    Image,
    Url,
    Unknown
};

/// A single clipboard history entry.
struct ClipEntry {
    uint64_t    id = 0;
    std::string content;        // text content or file path for images
    ClipType    type = ClipType::Text;
    std::string timestamp;      // ISO-8601
    bool        pinned = false;
    size_t      size_bytes = 0;
    std::string mime_type;
    std::string preview;        // truncated preview for display
};

/// Sync configuration for cross-device clipboard.
struct ClipSyncConfig {
    bool        enabled = false;
    std::string peer_address;
    int         port = 9812;
    std::string shared_key;
};

class ClipManager {
public:
    ClipManager();
    ~ClipManager();

    /// Read current clipboard content and add to history.
    Result<ClipEntry, std::string> paste() const;

    /// Copy text to clipboard and record in history.
    Result<void, std::string> copy(const std::string& text);

    /// Get clipboard history, optionally filtered by search query.
    Result<std::vector<ClipEntry>, std::string> history(const std::string& search = "",
                                                         int limit = 50) const;

    /// Pin a clipboard entry so it won't be auto-expired.
    Result<void, std::string> pin(uint64_t id);

    /// Unpin a clipboard entry.
    Result<void, std::string> unpin(uint64_t id);

    /// List pinned entries only.
    Result<std::vector<ClipEntry>, std::string> list_pinned() const;

    /// Clear all non-pinned history entries.
    Result<void, std::string> clear();

    /// Delete a specific entry by ID.
    Result<void, std::string> remove(uint64_t id);

    /// Auto-expire entries older than max_age_hours.
    Result<int, std::string> expire(int max_age_hours = 168);

    /// Get all entries for listing.
    Result<std::vector<ClipEntry>, std::string> list(int limit = 20) const;

private:
    std::string data_dir() const;
    std::string history_file() const;
    std::string images_dir() const;

    /// Detect content type from text.
    ClipType detect_type(const std::string& content) const;

    /// Generate a truncated preview string.
    std::string make_preview(const std::string& content, size_t max_len = 80) const;

    /// Load all entries from history file.
    std::vector<ClipEntry> load_entries() const;

    /// Save all entries to history file.
    void save_entries(const std::vector<ClipEntry>& entries) const;

    /// Get next available ID.
    uint64_t next_id(const std::vector<ClipEntry>& entries) const;

    /// Run a shell command and capture output.
    Result<std::string, std::string> run_cmd(const std::string& cmd) const;

    /// Get current ISO-8601 timestamp.
    std::string now_iso() const;

    /// Check if wayland session (for wl-paste/wl-copy).
    bool is_wayland() const;
};

} // namespace straylight
