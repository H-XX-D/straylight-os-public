// bin/fuse/operations.cpp
#include "operations.h"

#include <cerrno>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <unistd.h>

namespace straylight::fuse_fs {

FuseOps::FuseOps() : cache_(256 * 1024 * 1024) {} // 256MB cache

int FuseOps::getattr(const char* path, struct stat* st) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::memset(st, 0, sizeof(*st));

    std::string p(path);
    if (p == "/") {
        st->st_mode = S_IFDIR | 0755;
        st->st_nlink = 2;
        st->st_uid = getuid();
        st->st_gid = getgid();
        st->st_atime = st->st_mtime = st->st_ctime = time(nullptr);
        return 0;
    }

    auto it = files_.find(p);
    if (it == files_.end()) {
        return -ENOENT;
    }

    st->st_mode = S_IFREG | it->second.mode;
    st->st_nlink = 1;
    st->st_size = static_cast<off_t>(it->second.original_size);
    st->st_uid = getuid();
    st->st_gid = getgid();
    st->st_mtime = it->second.mtime;
    st->st_atime = st->st_ctime = it->second.mtime;
    return 0;
}

int FuseOps::readdir(const char* path, void* buf,
                     int (*filler)(void*, const char*, const struct stat*, off_t),
                     off_t /*offset*/) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string p(path);
    if (p != "/") {
        return -ENOENT;
    }

    filler(buf, ".", nullptr, 0);
    filler(buf, "..", nullptr, 0);

    for (const auto& [name, _] : files_) {
        // Strip leading slash.
        std::string display_name = name;
        if (!display_name.empty() && display_name[0] == '/') {
            display_name = display_name.substr(1);
        }
        filler(buf, display_name.c_str(), nullptr, 0);
    }

    return 0;
}

int FuseOps::open(const char* path, struct fuse_file_info* /*fi*/) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string p(path);
    if (files_.find(p) == files_.end()) {
        return -ENOENT;
    }
    return 0;
}

int FuseOps::read(const char* path, char* buf, size_t size, off_t offset,
                  struct fuse_file_info* /*fi*/) {
    std::string p(path);

    // Check cache first.
    auto cached = cache_.get(p, offset, size);
    if (cached.has_value()) {
        size_t to_copy = std::min(size, cached.value().size());
        std::memcpy(buf, cached.value().data(), to_copy);
        return static_cast<int>(to_copy);
    }

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = files_.find(p);
    if (it == files_.end()) {
        return -ENOENT;
    }

    // Decompress the data.
    std::vector<uint8_t> decompressed;
    if (it->second.is_tensor && !it->second.compressed_data.empty()) {
        auto result = compressor_.decompress(it->second.compressed_data);
        if (!result.has_value()) {
            return -EIO;
        }
        decompressed = std::move(result.value());
    } else {
        decompressed = it->second.compressed_data;
    }

    if (static_cast<size_t>(offset) >= decompressed.size()) {
        return 0;
    }

    size_t available = decompressed.size() - static_cast<size_t>(offset);
    size_t to_copy = std::min(size, available);
    std::memcpy(buf, decompressed.data() + offset, to_copy);

    // Cache the decompressed block.
    std::vector<uint8_t> cache_block(decompressed.data() + offset,
                                      decompressed.data() + offset + to_copy);
    cache_.put(p, offset, std::move(cache_block));

    return static_cast<int>(to_copy);
}

int FuseOps::write(const char* path, const char* buf, size_t size, off_t offset,
                   struct fuse_file_info* /*fi*/) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string p(path);
    auto it = files_.find(p);
    if (it == files_.end()) {
        return -ENOENT;
    }

    // For writes, we first decompress existing data, modify, then re-compress.
    std::vector<uint8_t> data;
    if (it->second.is_tensor && !it->second.compressed_data.empty()) {
        auto result = compressor_.decompress(it->second.compressed_data);
        if (result.has_value()) {
            data = std::move(result.value());
        }
    } else {
        data = it->second.compressed_data;
    }

    // Ensure data is large enough.
    size_t end_pos = static_cast<size_t>(offset) + size;
    if (data.size() < end_pos) {
        data.resize(end_pos, 0);
    }

    std::memcpy(data.data() + offset, buf, size);

    // Re-compress.
    CompressionType ctype = it->second.is_tensor ? CompressionType::DeltaZstd
                                                  : CompressionType::None;
    auto compressed = compressor_.compress(data.data(), data.size(), ctype);
    if (compressed.has_value()) {
        it->second.compressed_data = std::move(compressed.value());
        it->second.is_tensor = true;
    } else {
        it->second.compressed_data = data;
        it->second.is_tensor = false;
    }

    it->second.original_size = data.size();
    it->second.mtime = time(nullptr);

    // Invalidate cache.
    cache_.evict(p);

    return static_cast<int>(size);
}

int FuseOps::create(const char* path, mode_t mode, struct fuse_file_info* /*fi*/) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string p(path);
    if (files_.find(p) != files_.end()) {
        return -EEXIST;
    }

    VirtualFile vf;
    vf.name = p;
    vf.mode = mode & 0777;
    vf.mtime = time(nullptr);
    vf.original_size = 0;
    vf.is_tensor = (p.size() > 4 && p.substr(p.size() - 4) == ".slt");
    files_[p] = std::move(vf);

    return 0;
}

size_t FuseOps::file_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return files_.size();
}

void FuseOps::add_tensor_file(const std::string& path, VirtualFile vf) {
    std::lock_guard<std::mutex> lock(mutex_);
    files_[path] = std::move(vf);
}

} // namespace straylight::fuse_fs
