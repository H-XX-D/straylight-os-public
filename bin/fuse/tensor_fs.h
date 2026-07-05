// bin/fuse/tensor_fs.h
// FUSE low-level filesystem callbacks for the tensor compression filesystem.
// Serves compressed .tnsr files as transparently-decompressed content.
//
// Architecture:
//   - One FuseOps singleton (g_ops) is set during init().
//   - Static trampoline callbacks forward FUSE low-level requests to g_ops.
//   - Decompressed blocks are cached by (inode, block_idx) in a BlockCache.
//   - Backing files on disk are the .tnsr compressed format (tensor_format.h).
#pragma once

#define FUSE_USE_VERSION 35
#include <fuse3/fuse_lowlevel.h>

#include "compression.h"
#include "tensor_format.h"
#include "cache.h"

#include <straylight/result.h>

#include <cstdint>
#include <filesystem>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace straylight::fuse {

/// Constant block size for read/decompress operations (256 KiB).
inline constexpr size_t BLOCK_SIZE = 256UL * 1024;

/// On-disk block descriptor.
struct BlockIndex {
    uint64_t offset;           ///< Byte offset of compressed block in backing file
    uint32_t compressed_size;  ///< Compressed byte count
    uint32_t original_size;    ///< Decompressed byte count
    float    sparsity;         ///< Zero-element fraction [0,1]
};

/// Codec stored per-file in the tensor header.
using Codec = straylight::fuse_fs::CompressionType;

/// LRU cache key type: (inode number, block index within file).
struct CacheKey {
    uint64_t inode;
    uint32_t block_idx;
    bool operator==(const CacheKey& o) const noexcept {
        return inode == o.inode && block_idx == o.block_idx;
    }
};

struct CacheKeyHash {
    size_t operator()(const CacheKey& k) const noexcept {
        return std::hash<uint64_t>{}(k.inode) ^
               (static_cast<size_t>(k.block_idx) << 32);
    }
};

/// In-memory descriptor for a tensor file exposed via FUSE.
struct TensorFile {
    uint64_t                inode;
    std::string             name;
    std::filesystem::path   backing_path;  ///< On-disk .tnsr file
    uint64_t                apparent_size; ///< Uncompressed size in bytes
    straylight::fuse_fs::TensorMeta meta;  ///< Tensor header
    // Block index is derived on load from the backing file.
};

// ---------------------------------------------------------------------------
// LRU block cache typed for TensorFile blocks
// ---------------------------------------------------------------------------

class TensorBlockCache {
public:
    explicit TensorBlockCache(size_t max_bytes = 256UL * 1024 * 1024);

    /// Return cached decompressed block, or error on miss.
    Result<std::vector<uint8_t>, std::string> get(CacheKey key);

    /// Insert a decompressed block (LRU eviction when capacity exceeded).
    void put(CacheKey key, std::vector<uint8_t> data);

    /// Remove all entries for a given inode (called on file close/eviction).
    void evict(uint64_t inode);

    /// Clear the cache and apply a new maximum size.
    void reset(size_t max_bytes);

    [[nodiscard]] size_t current_bytes() const noexcept { return current_bytes_; }

private:
    size_t max_bytes_;
    size_t current_bytes_{0};
    mutable std::mutex mu_;

    struct Entry { CacheKey key; std::vector<uint8_t> data; };
    std::list<Entry> lru_;
    std::unordered_map<CacheKey, std::list<Entry>::iterator, CacheKeyHash> map_;

    void evict_one_lru(); // Must be called with mu_ held.
};

// ---------------------------------------------------------------------------
// FUSE operations class
// ---------------------------------------------------------------------------

class FuseOps {
public:
    /// Initialise the ops instance by scanning store_dir for .tnsr files.
    Result<void, std::string> init(const std::filesystem::path& store_dir,
                                   size_t cache_bytes);

    // -----------------------------------------------------------------------
    // Static FUSE low-level callbacks — forward to g_ops singleton.
    // -----------------------------------------------------------------------
    static void ll_lookup(fuse_req_t req, fuse_ino_t parent, const char* name);
    static void ll_getattr(fuse_req_t req, fuse_ino_t ino, fuse_file_info* fi);
    static void ll_open(fuse_req_t req, fuse_ino_t ino, fuse_file_info* fi);
    static void ll_release(fuse_req_t req, fuse_ino_t ino, fuse_file_info* fi);
    static void ll_read(fuse_req_t req, fuse_ino_t ino, size_t size,
                        off_t off, fuse_file_info* fi);
    static void ll_opendir(fuse_req_t req, fuse_ino_t ino, fuse_file_info* fi);
    static void ll_releasedir(fuse_req_t req, fuse_ino_t ino, fuse_file_info* fi);
    static void ll_readdir(fuse_req_t req, fuse_ino_t ino, size_t size,
                           off_t off, fuse_file_info* fi);

    /// Returns the populated fuse_lowlevel_ops struct for fuse_session_new().
    static const fuse_lowlevel_ops& get_ops();

private:
    std::filesystem::path store_dir_;
    TensorBlockCache      cache_{0};
    straylight::fuse_fs::TensorCompressor compressor_;
    straylight::fuse_fs::TensorFormat     format_;

    mutable std::shared_mutex              mu_;
    std::unordered_map<uint64_t, TensorFile> inodes_; ///< inode -> file descriptor
    std::unordered_map<std::string, uint64_t> names_; ///< filename -> inode
    uint64_t next_ino_{2}; ///< FUSE root inode is 1; files start at 2

    /// Register a .tnsr file and assign it an inode number.
    Result<void, std::string> register_file(const std::filesystem::path& path);

    /// Read and decompress one block from a TensorFile.
    Result<std::vector<uint8_t>, std::string>
    read_block(const TensorFile& tf, uint32_t block_idx);
};

} // namespace straylight::fuse
