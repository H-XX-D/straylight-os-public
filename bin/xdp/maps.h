// bin/xdp/maps.h
#pragma once

#include <straylight/result.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace straylight::xdp {

/// Manages BPF maps (hash, array, etc.) for XDP programs.
class BpfMapManager {
public:
    BpfMapManager() = default;
    ~BpfMapManager();

    BpfMapManager(const BpfMapManager&) = delete;
    BpfMapManager& operator=(const BpfMapManager&) = delete;

    /// Create a BPF hash map with the given parameters.
    Result<int, std::string> create_hash_map(const std::string& name,
                                             uint32_t key_size,
                                             uint32_t val_size,
                                             uint32_t max_entries);

    /// Update an entry in the named map.
    Result<void, std::string> update(const std::string& name,
                                     const void* key,
                                     const void* val);

    /// Lookup an entry by key; returns the value bytes.
    Result<std::vector<uint8_t>, std::string> lookup(const std::string& name,
                                                     const void* key);

    /// Delete an entry from the named map.
    Result<void, std::string> delete_entry(const std::string& name,
                                           const void* key);

    /// Get the fd for the named map, or -1 if not found.
    [[nodiscard]] int map_fd(const std::string& name) const;

private:
    struct MapInfo {
        int      fd        = -1;
        uint32_t key_size  = 0;
        uint32_t val_size  = 0;
    };

    std::unordered_map<std::string, MapInfo> maps_;
};

} // namespace straylight::xdp
