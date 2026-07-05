#pragma once

#include <straylight/export.h>
#include <straylight/ml/tensor.h>

#include <cstddef>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>

namespace straylight::ml {

/// LRU cache for KV tensors (inference state reuse).
class STRAYLIGHT_EXPORT KvCache {
public:
    explicit KvCache(size_t max_entries);

    /// Insert or replace a tensor in the cache.
    void put(const std::string& key, Tensor value);

    /// Retrieve a tensor. Returns nullptr on miss. Marks as recently used.
    Tensor* get(const std::string& key);

    /// Number of entries currently in cache.
    [[nodiscard]] size_t size() const noexcept {
        std::lock_guard lock(mu_);
        return map_.size();
    }

    /// Remove all entries.
    void clear();

private:
    void evict();

    mutable std::mutex mu_;
    size_t max_entries_;

    // LRU list: front = most recent, back = least recent
    using ListEntry = std::pair<std::string, Tensor>;
    std::list<ListEntry> lru_list_;
    std::unordered_map<std::string, std::list<ListEntry>::iterator> map_;
};

} // namespace straylight::ml
