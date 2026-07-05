// bin/fuse/cache.cpp
#include "cache.h"

namespace straylight::fuse_fs {

BlockCache::BlockCache(size_t max_bytes) : max_bytes_(max_bytes) {}

Result<std::vector<uint8_t>, std::string>
BlockCache::get(const std::string& path, off_t offset, size_t len) {
    std::lock_guard<std::mutex> lock(mutex_);

    CacheKey key{path, offset, len};
    auto it = map_.find(key);
    if (it == map_.end()) {
        ++misses_;
        return Result<std::vector<uint8_t>, std::string>::error("cache miss");
    }

    // Move to front of LRU list.
    lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
    ++hits_;

    return Result<std::vector<uint8_t>, std::string>::ok(it->second->data);
}

void BlockCache::put(const std::string& path, off_t offset, std::vector<uint8_t> data) {
    std::lock_guard<std::mutex> lock(mutex_);

    CacheKey key{path, offset, data.size()};

    // Check if already cached.
    auto it = map_.find(key);
    if (it != map_.end()) {
        // Update existing entry and move to front.
        current_bytes_ -= it->second->data.size();
        it->second->data = std::move(data);
        current_bytes_ += it->second->data.size();
        lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
        return;
    }

    size_t data_size = data.size();

    // Evict until we have room.
    while (current_bytes_ + data_size > max_bytes_ && !lru_list_.empty()) {
        evict_lru();
    }

    // Don't cache if a single block exceeds the max cache size.
    if (data_size > max_bytes_) return;

    // Insert at front.
    lru_list_.push_front({key, std::move(data)});
    map_[key] = lru_list_.begin();
    current_bytes_ += data_size;
}

void BlockCache::evict(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = lru_list_.begin();
    while (it != lru_list_.end()) {
        if (it->key.path == path) {
            current_bytes_ -= it->data.size();
            map_.erase(it->key);
            it = lru_list_.erase(it);
        } else {
            ++it;
        }
    }
}

size_t BlockCache::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_bytes_;
}

size_t BlockCache::block_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lru_list_.size();
}

void BlockCache::evict_lru() {
    // Caller must hold mutex_.
    if (lru_list_.empty()) return;

    auto& victim = lru_list_.back();
    current_bytes_ -= victim.data.size();
    map_.erase(victim.key);
    lru_list_.pop_back();
}

} // namespace straylight::fuse_fs
