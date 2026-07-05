#include <straylight/ml/kv_cache.h>

namespace straylight::ml {

KvCache::KvCache(size_t max_entries) : max_entries_(max_entries) {}

void KvCache::put(const std::string& key, Tensor value) {
    std::lock_guard lock(mu_);
    auto it = map_.find(key);
    if (it != map_.end()) {
        lru_list_.erase(it->second);
        map_.erase(it);
    }

    if (map_.size() >= max_entries_) {
        evict();
    }

    lru_list_.emplace_front(key, std::move(value));
    map_[key] = lru_list_.begin();
}

Tensor* KvCache::get(const std::string& key) {
    std::lock_guard lock(mu_);
    auto it = map_.find(key);
    if (it == map_.end()) return nullptr;

    // Move to front (most recently used)
    lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
    return &it->second->second;
}

void KvCache::clear() {
    std::lock_guard lock(mu_);
    lru_list_.clear();
    map_.clear();
}

void KvCache::evict() {
    if (lru_list_.empty()) return;
    auto& back = lru_list_.back();
    map_.erase(back.first);
    lru_list_.pop_back();
}

} // namespace straylight::ml
