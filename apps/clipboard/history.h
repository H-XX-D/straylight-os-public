// apps/clipboard/history.h
// Ring-buffer clipboard history: stores up to 500 entries (text or image),
// supports pinning, and persists the list to JSON on disk.
#pragma once

#include <straylight/result.h>
#include <straylight/error.h>

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace straylight::clipboard {

/// Data variant for a clipboard entry.
enum class EntryKind { Text, Image };

/// A single clipboard history entry.
struct ClipEntry {
    EntryKind   kind       = EntryKind::Text;
    std::string text;               ///< UTF-8 text content (if kind == Text)
    std::string mime;               ///< MIME type string, e.g. "text/plain" or "image/png"
    std::vector<uint8_t> image_data; ///< raw PNG bytes (if kind == Image)
    bool pinned = false;
    std::chrono::system_clock::time_point timestamp{};

    /// Short display label for the UI list.
    [[nodiscard]] std::string preview(size_t max_chars = 80) const;
};

/// Thread-safe ring buffer of clipboard entries.
/// Text entries are serialised to JSON for persistence.
/// Image entries are stored as base64-encoded PNG blobs in JSON.
class ClipHistory {
public:
    static constexpr size_t kMaxEntries = 500;

    /// Add a new text entry.  Does not add a duplicate of the current top entry.
    void push_text(std::string text, const std::string& mime = "text/plain");

    /// Add a new image entry (raw PNG bytes).
    void push_image(std::vector<uint8_t> png_data,
                    const std::string& mime = "image/png");

    /// Read-only snapshot of current entries (newest first).
    [[nodiscard]] std::vector<ClipEntry> entries() const;

    /// Number of entries currently stored.
    [[nodiscard]] size_t size() const;

    /// Toggle the pinned flag on an entry.
    void toggle_pin(size_t index);

    /// Remove an entry by index.
    void remove(size_t index);

    /// Clear all non-pinned entries.
    void clear_unpinned();

    /// Clear all entries including pinned.
    void clear_all();

    /// Persist the history to disk (text entries + pinned image entries).
    Result<void, SLError> save() const;

    /// Load persisted history from disk.
    Result<void, SLError> load();

    /// Path used for persistence.
    static std::filesystem::path storage_path();

private:
    mutable std::mutex mtx_;
    std::deque<ClipEntry> entries_; ///< front = newest
};

} // namespace straylight::clipboard
