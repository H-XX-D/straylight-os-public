// apps/browser/downloads.h
// StrayLight Browser — download queue with status tracking
#pragma once
#include <straylight/result.h>
#include <straylight/error.h>
#include <string>
#include <vector>
#include <cstdint>
#include <filesystem>

namespace straylight::browser {

enum class DownloadStatus { Pending, Active, Complete, Failed, Cancelled };

struct DownloadEntry {
    std::string     url;
    std::string     filename;
    std::string     dest_path;
    uint64_t        bytes_received = 0;
    uint64_t        bytes_total    = 0;
    DownloadStatus  status         = DownloadStatus::Pending;
    std::string     error_msg;
};

class Downloads {
public:
    /// Register a new download.  Returns the index in the queue.
    size_t add(const std::string& url, const std::string& dest_dir);

    /// Update byte counters for an active download.
    void update_progress(size_t index, uint64_t received, uint64_t total);

    /// Mark download as successfully finished.
    void mark_complete(size_t index);

    /// Mark download as failed with an error message.
    void mark_failed(size_t index, const std::string& error);

    /// Cancel an in-progress download.
    void cancel(size_t index);

    const std::vector<DownloadEntry>& entries() const { return entries_; }

    /// Draw the download manager panel.
    /// Returns true when the user clicked "Clear Completed".
    bool draw_panel();

    /// Persist download history to ~/.config/straylight/browser-downloads.json.
    Result<void, SLError> save_history() const;

    /// Load download history from the JSON file above.
    Result<void, SLError> load_history();

private:
    std::vector<DownloadEntry> entries_;

    static std::filesystem::path history_path();
    static std::string status_label(DownloadStatus s);
};

} // namespace straylight::browser
