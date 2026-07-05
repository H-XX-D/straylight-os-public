// bin/fuse/cache.h
// LRU block cache for decompressed tensor data
#pragma once

#include <straylight/result.h>

#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace straylight::fuse_fs {

class BlockCache {
public:
    explicit BlockCache(size_t max_bytes = 256 * 1024 * 1024); // 256MB default.

    /// Get cached data for a path + offset + length. Returns empty on miss.
    Result<std::vector<uint8_t>, std::string>
    get(const std::string& path, off_t offset, size_t len);

    /// Store data in the cache.
    void put(const std::string& path, off_t offset, std::vector<uint8_t> data);

    /// Evict all cache entries for a path.
    void evict(const std::string& path);

    /// Current total cached bytes.
    [[nodiscard]] size_t size() const;

    /// Total number of cached blocks.
    [[nodiscard]] size_t block_count() const;

    /// Cache hit statistics.
    [[nodiscard]] uint64_t hits() const { return hits_; }
    [[nodiscard]] uint64_t misses() const { return misses_; }

private:
    struct CacheKey {
        std::string path;
        off_t offset;
        size_t len;

        bool operator==(const CacheKey& other) const {
            return path == other.path && offset == other.offset && len == other.len;
        }
    };

    struct CacheKeyHash {
        size_t operator()(const CacheKey& k) const {
            size_t h1 = std::hash<std::string>{}(k.path);
            size_t h2 = std::hash<off_t>{}(k.offset);
            size_t h3 = std::hash<size_t>{}(k.len);
            return h1 ^ (h2 << 16) ^ (h3 << 32);
        }
    };

    struct CacheEntry {
        CacheKey key;
        std::vector<uint8_t> data;
    };

    size_t max_bytes_;
    size_t current_bytes_ = 0;
    uint64_t hits_ = 0;
    uint64_t misses_ = 0;

    // LRU list: front = most recently used, back = least recently used.
    std::list<CacheEntry> lru_list_;
    using LruIterator = std::list<CacheEntry>::iterator;
    std::unordered_map<CacheKey, LruIterator, CacheKeyHash> map_;

    mutable std::mutex mutex_;

    void evict_lru();
};

} // namespace straylight::fuse_fs
