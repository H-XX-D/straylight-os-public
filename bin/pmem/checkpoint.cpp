// bin/pmem/checkpoint.cpp
#include "checkpoint.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace straylight::pmem {

CheckpointManager::CheckpointManager(const std::string& base_dir)
    : base_dir_(base_dir) {}

std::string CheckpointManager::checkpoint_path(const std::string& name) const {
    return base_dir_ + "/" + name + ".ckpt";
}

Result<void, std::string> CheckpointManager::ensure_dir() const {
    struct stat st{};
    if (::stat(base_dir_.c_str(), &st) == 0) {
        if (S_ISDIR(st.st_mode)) return Result<void, std::string>::ok();
        return Result<void, std::string>::error(base_dir_ + " exists but is not a directory");
    }

    // Create directory hierarchy.
    std::string path;
    for (size_t i = 0; i < base_dir_.size(); ++i) {
        path += base_dir_[i];
        if (base_dir_[i] == '/' || i == base_dir_.size() - 1) {
            if (!path.empty() && path != "/") {
                ::mkdir(path.c_str(), 0755);
            }
        }
    }

    if (::stat(base_dir_.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        return Result<void, std::string>::error(
            "Cannot create directory " + base_dir_ + ": " + std::strerror(errno));
    }

    return Result<void, std::string>::ok();
}

Result<void, std::string> CheckpointManager::save(const std::string& name,
                                                     const void* data, size_t size) {
    if (name.empty()) {
        return Result<void, std::string>::error("Checkpoint name cannot be empty");
    }
    if (!data || size == 0) {
        return Result<void, std::string>::error("Cannot save empty checkpoint");
    }

    auto dir_result = ensure_dir();
    if (!dir_result.has_value()) {
        return dir_result;
    }

    std::string path = checkpoint_path(name);
    // Write to a temp file first, then rename for atomicity.
    std::string tmp_path = path + ".tmp";

    int fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return Result<void, std::string>::error(
            "Cannot create " + tmp_path + ": " + std::strerror(errno));
    }

    // Build and write header.
    CheckpointHeader hdr{};
    hdr.magic = CKPT_MAGIC;
    hdr.version = CKPT_VERSION;
    hdr.data_size = size;
    hdr.checksum = crc32(data, size);

    auto write_all = [](int fd, const void* buf, size_t count) -> bool {
        auto* p = reinterpret_cast<const uint8_t*>(buf);
        size_t written = 0;
        while (written < count) {
            ssize_t n = ::write(fd, p + written, count - written);
            if (n <= 0) return false;
            written += static_cast<size_t>(n);
        }
        return true;
    };

    bool ok = write_all(fd, &hdr, sizeof(hdr)) && write_all(fd, data, size);
    ::fsync(fd);
    ::close(fd);

    if (!ok) {
        ::unlink(tmp_path.c_str());
        return Result<void, std::string>::error("Write failed for checkpoint " + name);
    }

    // Atomic rename.
    if (::rename(tmp_path.c_str(), path.c_str()) != 0) {
        ::unlink(tmp_path.c_str());
        return Result<void, std::string>::error(
            "Rename failed: " + std::string(std::strerror(errno)));
    }

    return Result<void, std::string>::ok();
}

Result<std::vector<uint8_t>, std::string> CheckpointManager::load(const std::string& name) {
    std::string path = checkpoint_path(name);

    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return Result<std::vector<uint8_t>, std::string>::error(
            "Checkpoint '" + name + "' not found: " + std::strerror(errno));
    }

    // Read header.
    CheckpointHeader hdr{};
    ssize_t n = ::read(fd, &hdr, sizeof(hdr));
    if (n != static_cast<ssize_t>(sizeof(hdr))) {
        ::close(fd);
        return Result<std::vector<uint8_t>, std::string>::error(
            "Failed to read checkpoint header for '" + name + "'");
    }

    if (hdr.magic != CKPT_MAGIC) {
        ::close(fd);
        return Result<std::vector<uint8_t>, std::string>::error(
            "Invalid checkpoint magic for '" + name + "'");
    }
    if (hdr.version != CKPT_VERSION) {
        ::close(fd);
        return Result<std::vector<uint8_t>, std::string>::error(
            "Unsupported checkpoint version " + std::to_string(hdr.version));
    }

    // Read data.
    std::vector<uint8_t> data(hdr.data_size);
    size_t total_read = 0;
    while (total_read < hdr.data_size) {
        ssize_t r = ::read(fd, data.data() + total_read, hdr.data_size - total_read);
        if (r <= 0) {
            ::close(fd);
            return Result<std::vector<uint8_t>, std::string>::error(
                "Truncated checkpoint data for '" + name + "'");
        }
        total_read += static_cast<size_t>(r);
    }
    ::close(fd);

    // Verify checksum.
    uint32_t computed = crc32(data.data(), data.size());
    if (computed != hdr.checksum) {
        return Result<std::vector<uint8_t>, std::string>::error(
            "Checksum mismatch for checkpoint '" + name + "': stored=" +
            std::to_string(hdr.checksum) + " computed=" + std::to_string(computed));
    }

    return Result<std::vector<uint8_t>, std::string>::ok(std::move(data));
}

Result<std::vector<std::string>, std::string> CheckpointManager::list() {
    DIR* dir = ::opendir(base_dir_.c_str());
    if (!dir) {
        // Directory doesn't exist yet — no checkpoints.
        if (errno == ENOENT) {
            return Result<std::vector<std::string>, std::string>::ok({});
        }
        return Result<std::vector<std::string>, std::string>::error(
            "Cannot open " + base_dir_ + ": " + std::strerror(errno));
    }

    std::vector<std::string> names;
    struct dirent* ent;
    while ((ent = ::readdir(dir)) != nullptr) {
        std::string fname = ent->d_name;
        if (fname.size() > 5 && fname.substr(fname.size() - 5) == ".ckpt") {
            names.push_back(fname.substr(0, fname.size() - 5));
        }
    }
    ::closedir(dir);

    return Result<std::vector<std::string>, std::string>::ok(std::move(names));
}

Result<void, std::string> CheckpointManager::remove(const std::string& name) {
    std::string path = checkpoint_path(name);
    if (::unlink(path.c_str()) != 0) {
        if (errno == ENOENT) {
            return Result<void, std::string>::error(
                "Checkpoint '" + name + "' does not exist");
        }
        return Result<void, std::string>::error(
            "Cannot remove checkpoint '" + name + "': " + std::strerror(errno));
    }
    return Result<void, std::string>::ok();
}

uint32_t CheckpointManager::crc32(const void* data, size_t len) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int j = 0; j < 8; ++j) {
                c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
        return t;
    }();

    auto* p = reinterpret_cast<const uint8_t*>(data);
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

} // namespace straylight::pmem
