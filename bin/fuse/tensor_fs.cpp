// bin/fuse/tensor_fs.cpp
// FUSE low-level callbacks and tensor block cache implementation.

#include "tensor_fs.h"

#include <straylight/log.h>

#include <cerrno>
#include <cstring>
#include <fstream>

// Global singleton used by static FUSE callbacks.
static straylight::fuse::FuseOps* g_ops = nullptr;

namespace straylight::fuse {

// ============================================================================
// TensorBlockCache
// ============================================================================

TensorBlockCache::TensorBlockCache(size_t max_bytes)
    : max_bytes_(max_bytes) {}

Result<std::vector<uint8_t>, std::string>
TensorBlockCache::get(CacheKey key) {
    std::lock_guard lk(mu_);
    auto it = map_.find(key);
    if (it == map_.end()) {
        return Result<std::vector<uint8_t>, std::string>::error("miss");
    }
    // Promote to front.
    lru_.splice(lru_.begin(), lru_, it->second);
    return Result<std::vector<uint8_t>, std::string>::ok(it->second->data);
}

void TensorBlockCache::put(CacheKey key, std::vector<uint8_t> data) {
    std::lock_guard lk(mu_);
    // Remove existing entry for this key if present.
    if (auto it = map_.find(key); it != map_.end()) {
        current_bytes_ -= it->second->data.size();
        lru_.erase(it->second);
        map_.erase(it);
    }
    // Evict until we have room.
    while (!lru_.empty() && current_bytes_ + data.size() > max_bytes_) {
        evict_one_lru();
    }
    current_bytes_ += data.size();
    lru_.push_front({key, std::move(data)});
    map_[key] = lru_.begin();
}

void TensorBlockCache::evict(uint64_t inode) {
    std::lock_guard lk(mu_);
    for (auto it = lru_.begin(); it != lru_.end(); ) {
        if (it->key.inode == inode) {
            current_bytes_ -= it->data.size();
            map_.erase(it->key);
            it = lru_.erase(it);
        } else {
            ++it;
        }
    }
}

void TensorBlockCache::reset(size_t max_bytes) {
    std::lock_guard lk(mu_);
    max_bytes_ = max_bytes;
    current_bytes_ = 0;
    lru_.clear();
    map_.clear();
}

void TensorBlockCache::evict_one_lru() {
    if (lru_.empty()) return;
    auto& victim = lru_.back();
    current_bytes_ -= victim.data.size();
    map_.erase(victim.key);
    lru_.pop_back();
}

// ============================================================================
// FuseOps::init — scan store_dir, register all .slt files
// ============================================================================

Result<void, std::string>
FuseOps::init(const std::filesystem::path& store_dir, size_t cache_bytes) {
    store_dir_ = store_dir;
    cache_.reset(cache_bytes);
    g_ops      = this;

    // Create the store directory if it doesn't exist.
    std::error_code ec;
    std::filesystem::create_directories(store_dir_, ec);
    if (ec) {
        return Result<void, std::string>::error(
            "Cannot create store dir " + store_dir_.string() +
            ": " + ec.message());
    }

    // Scan for existing .slt tensor files.
    for (auto& entry : std::filesystem::directory_iterator(store_dir_, ec)) {
        if (ec) break;
        if (entry.is_regular_file() && entry.path().extension() == ".slt") {
            auto r = register_file(entry.path());
            if (!r.has_value()) {
                SL_WARN("fuse: skipping {}: {}", entry.path().string(), r.error());
            }
        }
    }

    SL_INFO("fuse: store={} files={} cache_bytes={}",
            store_dir_.string(), inodes_.size(), cache_bytes);
    return Result<void, std::string>::ok();
}

Result<void, std::string>
FuseOps::register_file(const std::filesystem::path& path) {
    // Read the file header to determine apparent size.
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        return Result<void, std::string>::error("Cannot open: " + path.string());
    }

    std::vector<uint8_t> header_buf(1024);
    f.read(reinterpret_cast<char*>(header_buf.data()),
           static_cast<std::streamsize>(header_buf.size()));
    header_buf.resize(static_cast<size_t>(f.gcount()));

    auto meta_result = format_.parse_header(header_buf);
    if (!meta_result.has_value()) {
        return Result<void, std::string>::error(
            "Bad header in " + path.string() + ": " + meta_result.error());
    }

    TensorFile tf;
    tf.inode        = next_ino_++;
    tf.name         = path.filename().string();
    tf.backing_path = path;
    tf.apparent_size = meta_result.value().original_size;
    tf.meta         = std::move(meta_result.value());

    const std::string fname = tf.name;
    const uint64_t    ino   = tf.inode;
    inodes_[ino]   = std::move(tf);
    names_[fname]  = ino;
    return Result<void, std::string>::ok();
}

// ============================================================================
// Internal block reader
// ============================================================================

Result<std::vector<uint8_t>, std::string>
FuseOps::read_block(const TensorFile& tf, uint32_t /*block_idx*/) {
    // Cache check.
    CacheKey ck{tf.inode, 0};
    if (auto hit = cache_.get(ck); hit.has_value()) {
        return hit;
    }

    // Read the full compressed payload from the backing file.
    std::ifstream f(tf.backing_path, std::ios::binary);
    if (!f.is_open()) {
        return Result<std::vector<uint8_t>, std::string>::error(
            "Cannot open backing file: " + tf.backing_path.string());
    }

    // Skip the header (data starts at meta.data_offset).
    f.seekg(static_cast<std::streamoff>(tf.meta.data_offset));

    std::vector<uint8_t> compressed(
        std::istreambuf_iterator<char>(f),
        std::istreambuf_iterator<char>{});

    // Decompress.
    auto decompressed = compressor_.decompress(compressed);
    if (!decompressed.has_value()) {
        return Result<std::vector<uint8_t>, std::string>::error(
            "Decompression failed: " + decompressed.error());
    }

    cache_.put(ck, decompressed.value());
    return decompressed;
}

// ============================================================================
// FUSE low-level callback implementations
// ============================================================================

void FuseOps::ll_getattr(fuse_req_t req, fuse_ino_t ino, fuse_file_info* /*fi*/) {
    struct stat st{};

    if (ino == FUSE_ROOT_ID) {
        // Root directory.
        st.st_ino   = FUSE_ROOT_ID;
        st.st_mode  = S_IFDIR | 0755;
        st.st_nlink = 2;
        fuse_reply_attr(req, &st, 1.0);
        return;
    }

    std::shared_lock lk(g_ops->mu_);
    auto it = g_ops->inodes_.find(static_cast<uint64_t>(ino));
    if (it == g_ops->inodes_.end()) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    const auto& tf = it->second;
    st.st_ino   = ino;
    st.st_mode  = S_IFREG | 0644;
    st.st_nlink = 1;
    st.st_size  = static_cast<off_t>(tf.apparent_size);
    fuse_reply_attr(req, &st, 1.0);
}

void FuseOps::ll_lookup(fuse_req_t req, fuse_ino_t parent, const char* name) {
    if (parent != FUSE_ROOT_ID) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    std::shared_lock lk(g_ops->mu_);
    auto nit = g_ops->names_.find(name);
    if (nit == g_ops->names_.end()) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    uint64_t ino = nit->second;
    auto iit = g_ops->inodes_.find(ino);
    if (iit == g_ops->inodes_.end()) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    struct fuse_entry_param e{};
    e.ino              = static_cast<fuse_ino_t>(ino);
    e.attr_timeout     = 1.0;
    e.entry_timeout    = 1.0;
    e.attr.st_ino      = e.ino;
    e.attr.st_mode     = S_IFREG | 0644;
    e.attr.st_nlink    = 1;
    e.attr.st_size     = static_cast<off_t>(iit->second.apparent_size);
    fuse_reply_entry(req, &e);
}

void FuseOps::ll_open(fuse_req_t req, fuse_ino_t ino, fuse_file_info* fi) {
    std::shared_lock lk(g_ops->mu_);
    if (g_ops->inodes_.find(static_cast<uint64_t>(ino)) == g_ops->inodes_.end()) {
        fuse_reply_err(req, ENOENT);
        return;
    }
    // Only read-only access allowed.
    if ((fi->flags & O_ACCMODE) != O_RDONLY) {
        fuse_reply_err(req, EACCES);
        return;
    }
    fuse_reply_open(req, fi);
}

void FuseOps::ll_release(fuse_req_t req, fuse_ino_t /*ino*/, fuse_file_info* /*fi*/) {
    fuse_reply_err(req, 0);
}

void FuseOps::ll_read(fuse_req_t req, fuse_ino_t ino, size_t size,
                       off_t off, fuse_file_info* /*fi*/) {
    std::shared_lock lk(g_ops->mu_);
    auto it = g_ops->inodes_.find(static_cast<uint64_t>(ino));
    if (it == g_ops->inodes_.end()) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    const auto& tf = it->second;
    if (static_cast<uint64_t>(off) >= tf.apparent_size) {
        // Read beyond EOF: return empty buffer.
        fuse_reply_buf(req, nullptr, 0);
        return;
    }

    // Determine which 256-KiB block contains this offset.
    const uint32_t block_idx = static_cast<uint32_t>(
        static_cast<uint64_t>(off) / BLOCK_SIZE);
    const size_t block_off = static_cast<size_t>(
        static_cast<uint64_t>(off) % BLOCK_SIZE);

    lk.unlock(); // Release read lock before potentially expensive I/O.
    auto data = g_ops->read_block(tf, block_idx);
    if (!data.has_value()) {
        SL_ERROR("fuse read_block failed: {}", data.error());
        fuse_reply_err(req, EIO);
        return;
    }

    const auto& block = data.value();
    if (block_off >= block.size()) {
        fuse_reply_buf(req, nullptr, 0);
        return;
    }

    const size_t avail = block.size() - block_off;
    const size_t n     = std::min(size, avail);
    fuse_reply_buf(req,
                   reinterpret_cast<const char*>(block.data() + block_off),
                   n);
}

void FuseOps::ll_opendir(fuse_req_t req, fuse_ino_t ino, fuse_file_info* fi) {
    if (ino != FUSE_ROOT_ID) {
        fuse_reply_err(req, ENOTDIR);
        return;
    }
    fuse_reply_open(req, fi);
}

void FuseOps::ll_releasedir(fuse_req_t req, fuse_ino_t /*ino*/, fuse_file_info* /*fi*/) {
    fuse_reply_err(req, 0);
}

void FuseOps::ll_readdir(fuse_req_t req, fuse_ino_t ino, size_t size,
                          off_t off, fuse_file_info* fi) {
    (void)fi;
    if (ino != FUSE_ROOT_ID) {
        fuse_reply_err(req, ENOTDIR);
        return;
    }

    std::shared_lock lk(g_ops->mu_);

    // Build a snapshot of directory entries: "." + ".." + files.
    std::vector<std::pair<std::string, fuse_ino_t>> entries;
    entries.push_back({".", FUSE_ROOT_ID});
    entries.push_back({"..", FUSE_ROOT_ID});
    for (const auto& [fname, fino] : g_ops->names_) {
        entries.push_back({fname, fino});
    }
    lk.unlock();

    // Allocate a buffer and fill entries starting at 'off'.
    std::vector<char> buf(size);
    size_t buf_used = 0;

    for (size_t idx = static_cast<size_t>(off); idx < entries.size(); ++idx) {
        const auto& [ename, eino] = entries[idx];
        struct stat st{};
        st.st_ino  = eino;
        st.st_mode = (eino == FUSE_ROOT_ID) ? (S_IFDIR | 0755) : (S_IFREG | 0644);

        size_t entry_size = fuse_add_direntry(
            req,
            buf.data() + buf_used,
            size - buf_used,
            ename.c_str(),
            &st,
            static_cast<off_t>(idx + 1));

        if (buf_used + entry_size > size) break;
        buf_used += entry_size;
    }

    fuse_reply_buf(req, buf.data(), buf_used);
}

// ============================================================================
// Static ops table
// ============================================================================

const fuse_lowlevel_ops& FuseOps::get_ops() {
    static fuse_lowlevel_ops ops{};
    ops.lookup  = ll_lookup;
    ops.getattr = ll_getattr;
    ops.open    = ll_open;
    ops.release = ll_release;
    ops.read    = ll_read;
    ops.opendir = ll_opendir;
    ops.releasedir = ll_releasedir;
    ops.readdir = ll_readdir;
    return ops;
}

} // namespace straylight::fuse
