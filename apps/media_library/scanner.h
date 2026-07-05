// apps/media_library/scanner.h
// Recursive filesystem scanner: detects MIME type, extracts basic metadata
// for images, audio, and video files.
#pragma once

#include <straylight/result.h>
#include <straylight/error.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace straylight::media {

namespace fs = std::filesystem;

/// Media category derived from MIME type.
enum class MediaType { Image, Audio, Video, Unknown };

/// Metadata extracted for a single media file.
struct MediaEntry {
    fs::path    path;
    MediaType   type       = MediaType::Unknown;
    std::string mime;               ///< e.g. "image/jpeg"
    std::string title;
    std::string artist;
    std::string album;
    std::string genre;
    int         year       = 0;
    int         track      = 0;
    uint64_t    duration_ms = 0;    ///< milliseconds; 0 if not applicable
    uint32_t    width       = 0;    ///< pixels for images/video
    uint32_t    height      = 0;
    uint64_t    file_size   = 0;    ///< bytes
    std::chrono::system_clock::time_point mtime{};
};

/// Callback invoked for each file discovered during a scan.
using ScanCallback = std::function<void(MediaEntry)>;

/// Scans one or more directories recursively, classifies media files,
/// and extracts their metadata.
class Scanner {
public:
    /// Maximum file size to attempt metadata extraction (default: 4 GB).
    uint64_t max_file_bytes = 4ULL * 1024 * 1024 * 1024;

    /// Scan `root` recursively, invoking `cb` for every media file found.
    /// Returns the number of files processed.
    Result<size_t, SLError> scan(const fs::path& root, ScanCallback cb);

    /// Classify a single file and extract its metadata without a callback.
    Result<MediaEntry, SLError> probe(const fs::path& path);

private:
    /// Determine the MIME type by reading the file header (magic bytes).
    static std::string detect_mime(const fs::path& path);

    /// Map a MIME type string to a MediaType enum.
    static MediaType mime_to_type(const std::string& mime);

    /// Extract audio metadata tags via GStreamer discoverer.
    void extract_audio_meta(MediaEntry& e);

    /// Extract video dimensions + duration via GStreamer discoverer.
    void extract_video_meta(MediaEntry& e);

    /// Extract image dimensions from file header (no full decode).
    static void extract_image_meta(MediaEntry& e);
};

} // namespace straylight::media
