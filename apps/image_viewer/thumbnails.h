// apps/image_viewer/thumbnails.h
// Directory scan + thumbnail grid with LRU GL texture cache.
#pragma once

#include "loader.h"

#include <straylight/result.h>
#include <straylight/error.h>

#include <filesystem>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

namespace straylight::viewer {

/// A thumbnail entry in the grid.
struct ThumbEntry {
    std::filesystem::path path;
    GLuint texture_id = 0;  ///< 0 = not yet generated
    uint32_t width    = 0;
    uint32_t height   = 0;
    bool     loaded   = false;
    bool     failed   = false;
};

/// LRU texture cache: keeps the most recently used N thumbnails in GPU memory.
class LruThumbCache {
public:
    explicit LruThumbCache(size_t max_entries = 256);
    ~LruThumbCache();

    /// Return texture_id for `path`, or 0 if not cached.
    GLuint get(const std::filesystem::path& path);

    /// Insert a texture into the cache.
    void   put(const std::filesystem::path& path, GLuint texture_id,
               uint32_t w, uint32_t h);

    void   evict_all();
    size_t size() const { return map_.size(); }

private:
    struct CacheEntry {
        std::filesystem::path path;
        GLuint texture_id = 0;
    };

    size_t max_entries_;
    std::list<CacheEntry>                                                      lru_list_;
    std::unordered_map<std::string, std::list<CacheEntry>::iterator>           map_;

    void evict_one();
};

/// Scans a directory and builds a thumbnail grid, generating GL textures
/// lazily on demand (one per frame to avoid stalls).
class ThumbnailGrid {
public:
    static constexpr uint32_t kThumbSize = 128;

    explicit ThumbnailGrid(size_t cache_capacity = 256);

    /// Scan `dir` and populate the entry list (async-safe: just collects paths).
    Result<size_t, SLError> scan(const std::filesystem::path& dir);

    /// Generate at most `budget` pending thumbnails; call each frame.
    void generate_pending(int budget = 2);

    const std::vector<ThumbEntry>& entries() const { return entries_; }
    int count() const { return static_cast<int>(entries_.size()); }

    /// Draw the thumbnail grid inside the current ImGui child window.
    /// Returns the index of the item clicked, or -1.
    int draw_grid(float panel_width, float thumb_display_size = 100.0f) const;

    /// Clear all entries and release GPU textures.
    void clear();

private:
    std::vector<ThumbEntry> entries_;
    LruThumbCache           cache_;

    /// Generate thumbnail for entries_[index]. Uploads texture and updates entry.
    void generate(size_t index);
};

} // namespace straylight::viewer
