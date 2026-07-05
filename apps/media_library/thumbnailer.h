// apps/media_library/thumbnailer.h
// Thumbnail generation for images (stb_image + stb_image_resize) and
// video frames (GStreamer pipeline seek + appsink).
#pragma once

#include "scanner.h"

#include <straylight/result.h>
#include <straylight/error.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace straylight::media {

/// An RGBA thumbnail buffer with its dimensions.
struct Thumbnail {
    std::vector<uint8_t> rgba;   ///< raw pixels, row-major, 4 bytes per pixel
    uint32_t             width  = 0;
    uint32_t             height = 0;
};

/// Generates thumbnails and caches them on disk as PNG files under
/// `~/.cache/straylight/media-thumbs/`.
class Thumbnailer {
public:
    /// Maximum dimension of generated thumbnails (both axes).
    uint32_t thumb_size = 256;

    /// Generate (or load from cache) a thumbnail for the given media entry.
    /// Returns an RGBA pixel buffer ready to upload to an OpenGL texture.
    Result<Thumbnail, SLError> get(const MediaEntry& e);

    /// Evict the on-disk cache entry for a given media path.
    void evict(const MediaEntry& e);

    /// Return the cache directory path.
    static std::filesystem::path cache_dir();

private:
    /// Derive a deterministic cache filename from the media path.
    static std::filesystem::path cache_path(const MediaEntry& e);

    /// Load a PNG from the cache directory into a Thumbnail.
    static Result<Thumbnail, SLError> load_cached(
        const std::filesystem::path& cache_file);

    /// Save an RGBA buffer as a PNG to the cache directory.
    static Result<void, SLError> save_cached(
        const std::filesystem::path& cache_file, const Thumbnail& t);

    /// Decode and resize an image file using stb_image + stb_image_resize.
    Result<Thumbnail, SLError> thumbnail_image(const MediaEntry& e);

    /// Extract a representative video frame via GStreamer and resize it.
    Result<Thumbnail, SLError> thumbnail_video(const MediaEntry& e);
};

} // namespace straylight::media
