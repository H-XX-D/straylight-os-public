// bin/xdp/maps.cpp
#include "maps.h"

#include <linux/bpf.h>   // must come before bpf/bpf.h to define bpf_stats_type
#include <bpf/bpf.h>

#include <cerrno>
#include <cstring>
#include <unistd.h>

namespace straylight::xdp {

BpfMapManager::~BpfMapManager() {
    for (auto& [name, info] : maps_) {
        if (info.fd >= 0) {
            ::close(info.fd);
        }
    }
}

Result<int, std::string> BpfMapManager::create_hash_map(const std::string& name,
                                                        uint32_t key_size,
                                                        uint32_t val_size,
                                                        uint32_t max_entries) {
    if (maps_.count(name)) {
        return Result<int, std::string>::error("Map '" + name + "' already exists");
    }

    // libbpf 0.5 uses bpf_create_map_name; 0.7+ added bpf_map_create
#if defined(LIBBPF_MAJOR_VERSION) && (LIBBPF_MAJOR_VERSION > 0 || LIBBPF_MINOR_VERSION >= 7)
    LIBBPF_OPTS(bpf_map_create_opts, opts);
    int fd = bpf_map_create(BPF_MAP_TYPE_HASH, name.c_str(),
                            key_size, val_size, max_entries, &opts);
#else
    int fd = bpf_create_map_name(BPF_MAP_TYPE_HASH, name.c_str(),
                                  static_cast<int>(key_size),
                                  static_cast<int>(val_size),
                                  static_cast<int>(max_entries), 0);
#endif
    if (fd < 0) {
        return Result<int, std::string>::error(
            "Failed to create map '" + name + "': " + std::strerror(errno));
    }

    maps_[name] = MapInfo{fd, key_size, val_size};
    return Result<int, std::string>::ok(fd);
}

Result<void, std::string> BpfMapManager::update(const std::string& name,
                                                const void* key,
                                                const void* val) {
    auto it = maps_.find(name);
    if (it == maps_.end()) {
        return Result<void, std::string>::error("Map '" + name + "' not found");
    }

    int err = bpf_map_update_elem(it->second.fd, key, val, BPF_ANY);
    if (err) {
        return Result<void, std::string>::error(
            "Failed to update map '" + name + "': " + std::strerror(errno));
    }

    return Result<void, std::string>::ok();
}

Result<std::vector<uint8_t>, std::string> BpfMapManager::lookup(const std::string& name,
                                                                 const void* key) {
    auto it = maps_.find(name);
    if (it == maps_.end()) {
        return Result<std::vector<uint8_t>, std::string>::error(
            "Map '" + name + "' not found");
    }

    std::vector<uint8_t> value(it->second.val_size);
    int err = bpf_map_lookup_elem(it->second.fd, key, value.data());
    if (err) {
        return Result<std::vector<uint8_t>, std::string>::error(
            "Lookup failed in map '" + name + "': " + std::strerror(errno));
    }

    return Result<std::vector<uint8_t>, std::string>::ok(std::move(value));
}

Result<void, std::string> BpfMapManager::delete_entry(const std::string& name,
                                                       const void* key) {
    auto it = maps_.find(name);
    if (it == maps_.end()) {
        return Result<void, std::string>::error("Map '" + name + "' not found");
    }

    int err = bpf_map_delete_elem(it->second.fd, key);
    if (err) {
        return Result<void, std::string>::error(
            "Delete failed in map '" + name + "': " + std::strerror(errno));
    }

    return Result<void, std::string>::ok();
}

int BpfMapManager::map_fd(const std::string& name) const {
    auto it = maps_.find(name);
    return (it != maps_.end()) ? it->second.fd : -1;
}

} // namespace straylight::xdp
