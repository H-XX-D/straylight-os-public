// bin/fuse/operations.h
// FUSE filesystem operations for the tensor compression filesystem
#pragma once

#include "cache.h"
#include "compression.h"
#include "tensor_format.h"

#include <straylight/result.h>

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// Forward-declare FUSE types to avoid requiring fuse3 headers in all translation units.
struct stat;
struct fuse_file_info;

namespace straylight::fuse_fs {

/// In-memory representation of a file in our FUSE filesystem.
struct VirtualFile {
    std::string name;
    std::vector<uint8_t> compressed_data;  // Stored on disk in compressed form.
    TensorMeta meta;                        // Tensor metadata.
    size_t original_size = 0;               // Uncompressed size.
    bool is_tensor = false;                 // True if this is a .slt tensor file.
    mode_t mode = 0100644;                  // File permissions.
    time_t mtime = 0;                       // Last modification time.
};

/// FUSE filesystem operations backed by in-memory tensor storage.
class FuseOps {
public:
    FuseOps();

    /// FUSE getattr: stat a file or directory.
    int getattr(const char* path, struct stat* st);

    /// FUSE readdir: list directory contents.
    int readdir(const char* path, void* buf,
                int (*filler)(void*, const char*, const struct stat*, off_t),
                off_t offset);

    /// FUSE open: validate file exists and is readable.
    int open(const char* path, struct fuse_file_info* fi);

    /// FUSE read: read decompressed data from a file.
    int read(const char* path, char* buf, size_t size, off_t offset,
             struct fuse_file_info* fi);

    /// FUSE write: write data (compresses on close).
    int write(const char* path, const char* buf, size_t size, off_t offset,
              struct fuse_file_info* fi);

    /// FUSE create: create a new file.
    int create(const char* path, mode_t mode, struct fuse_file_info* fi);

    /// Get pointer to the shared block cache.
    BlockCache& cache() { return cache_; }

    /// Get number of stored files.
    size_t file_count() const;

    /// Add a pre-compressed tensor file (for testing).
    void add_tensor_file(const std::string& path, VirtualFile vf);

private:
    std::map<std::string, VirtualFile> files_;
    mutable std::mutex mutex_;
    BlockCache cache_;
    TensorCompressor compressor_;
    TensorFormat format_;
};

} // namespace straylight::fuse_fs
