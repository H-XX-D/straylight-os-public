// apps/browser/downloads.cpp
// StrayLight Browser — download manager implementation
#include "downloads.h"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace straylight::browser {

namespace fs = std::filesystem;
using json   = nlohmann::json;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

fs::path Downloads::history_path() {
    const char* home = std::getenv("HOME");
    fs::path base = home ? fs::path(home) / ".config" / "straylight"
                         : fs::path("/tmp/straylight");
    return base / "browser-downloads.json";
}

std::string Downloads::status_label(DownloadStatus s) {
    switch (s) {
    case DownloadStatus::Pending:   return "Pending";
    case DownloadStatus::Active:    return "Downloading";
    case DownloadStatus::Complete:  return "Done";
    case DownloadStatus::Failed:    return "Failed";
    case DownloadStatus::Cancelled: return "Cancelled";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// Queue management
// ---------------------------------------------------------------------------

size_t Downloads::add(const std::string& url, const std::string& dest_dir) {
    DownloadEntry entry;
    entry.url    = url;
    entry.status = DownloadStatus::Pending;

    // Extract filename from URL
    auto pos = url.rfind('/');
    entry.filename = (pos != std::string::npos && pos + 1 < url.size())
                     ? url.substr(pos + 1) : "download";

    // Strip query string from filename
    auto q = entry.filename.find('?');
    if (q != std::string::npos) entry.filename = entry.filename.substr(0, q);

    entry.dest_path = (fs::path(dest_dir) / entry.filename).string();

    entries_.push_back(std::move(entry));
    return entries_.size() - 1;
}

void Downloads::update_progress(size_t index, uint64_t received, uint64_t total) {
    if (index >= entries_.size()) return;
    entries_[index].bytes_received = received;
    entries_[index].bytes_total    = total;
    entries_[index].status         = DownloadStatus::Active;
}

void Downloads::mark_complete(size_t index) {
    if (index >= entries_.size()) return;
    entries_[index].status = DownloadStatus::Complete;
    entries_[index].bytes_received = entries_[index].bytes_total;
}

void Downloads::mark_failed(size_t index, const std::string& error) {
    if (index >= entries_.size()) return;
    entries_[index].status    = DownloadStatus::Failed;
    entries_[index].error_msg = error;
}

void Downloads::cancel(size_t index) {
    if (index >= entries_.size()) return;
    if (entries_[index].status == DownloadStatus::Active ||
        entries_[index].status == DownloadStatus::Pending) {
        entries_[index].status = DownloadStatus::Cancelled;
    }
}

// ---------------------------------------------------------------------------
// ImGui panel
// ---------------------------------------------------------------------------

bool Downloads::draw_panel() {
    bool clear_requested = false;

    ImGui::Text("Downloads (%zu)", entries_.size());
    ImGui::Separator();

    for (size_t i = 0; i < entries_.size(); ++i) {
        auto& e = entries_[i];
        ImGui::PushID(static_cast<int>(i));

        // Filename
        ImGui::TextUnformatted(e.filename.c_str());

        // Progress bar for active downloads
        if (e.status == DownloadStatus::Active && e.bytes_total > 0) {
            float frac = static_cast<float>(e.bytes_received) /
                         static_cast<float>(e.bytes_total);
            char overlay[32];
            snprintf(overlay, sizeof(overlay), "%.0f%%", frac * 100.0f);
            ImGui::ProgressBar(frac, ImVec2(-1.0f, 0.0f), overlay);
        }

        // Status + byte info on one line
        float kb_recv = static_cast<float>(e.bytes_received) / 1024.0f;
        float kb_total = static_cast<float>(e.bytes_total) / 1024.0f;
        ImGui::TextDisabled("%s  %.1f / %.1f KB",
                            status_label(e.status).c_str(),
                            kb_recv, kb_total);

        // Error message
        if (!e.error_msg.empty()) {
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Error: %s",
                               e.error_msg.c_str());
        }

        // Cancel button for active/pending
        if (e.status == DownloadStatus::Active ||
            e.status == DownloadStatus::Pending) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Cancel"))
                cancel(i);
        }

        ImGui::Separator();
        ImGui::PopID();
    }

    if (ImGui::Button("Clear Completed")) {
        std::erase_if(entries_, [](const DownloadEntry& e) {
            return e.status == DownloadStatus::Complete ||
                   e.status == DownloadStatus::Cancelled;
        });
        clear_requested = true;
    }

    return clear_requested;
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

Result<void, SLError> Downloads::save_history() const {
    fs::path path = history_path();
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError, "cannot create config dir: " + ec.message()});
    }

    json arr = json::array();
    for (const auto& e : entries_) {
        arr.push_back({
            {"url",            e.url},
            {"filename",       e.filename},
            {"dest_path",      e.dest_path},
            {"bytes_received", e.bytes_received},
            {"bytes_total",    e.bytes_total},
            {"status",         static_cast<int>(e.status)},
            {"error_msg",      e.error_msg},
        });
    }

    std::ofstream f(path);
    if (!f) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError, "cannot write: " + path.string()});
    }
    f << arr.dump(2);
    return Result<void, SLError>::ok();
}

Result<void, SLError> Downloads::load_history() {
    fs::path path = history_path();
    if (!fs::exists(path)) return Result<void, SLError>::ok();

    std::ifstream f(path);
    if (!f) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError, "cannot read: " + path.string()});
    }

    try {
        json arr = json::parse(f);
        for (const auto& item : arr) {
            DownloadEntry e;
            e.url            = item.value("url", "");
            e.filename       = item.value("filename", "");
            e.dest_path      = item.value("dest_path", "");
            e.bytes_received = item.value("bytes_received", uint64_t{0});
            e.bytes_total    = item.value("bytes_total",    uint64_t{0});
            e.status         = static_cast<DownloadStatus>(
                                   item.value("status", 0));
            e.error_msg      = item.value("error_msg", "");
            entries_.push_back(std::move(e));
        }
    } catch (const json::exception& ex) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::ParseError, ex.what()});
    }

    return Result<void, SLError>::ok();
}

} // namespace straylight::browser
