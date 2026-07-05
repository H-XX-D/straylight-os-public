// bin/pmem/checkpoint.h
// Named tensor checkpoints on persistent memory
#pragma once

#include <straylight/result.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace straylight::pmem {

class CheckpointManager {
public:
    explicit CheckpointManager(const std::string& base_dir = "/var/lib/straylight/checkpoints");

    /// Save a named checkpoint.
    Result<void, std::string> save(const std::string& name, const void* data, size_t size);

    /// Load a named checkpoint.
    Result<std::vector<uint8_t>, std::string> load(const std::string& name);

    /// List all available checkpoints.
    Result<std::vector<std::string>, std::string> list();

    /// Remove a named checkpoint.
    Result<void, std::string> remove(const std::string& name);

private:
    std::string base_dir_;

    /// Get the file path for a checkpoint name.
    std::string checkpoint_path(const std::string& name) const;

    /// Ensure the base directory exists.
    Result<void, std::string> ensure_dir() const;

    // On-disk checkpoint format:
    struct CheckpointHeader {
        uint64_t magic;
        uint64_t version;
        uint64_t data_size;
        uint32_t checksum;
        uint32_t padding;
    };

    static constexpr uint64_t CKPT_MAGIC = 0x53544C434B505401ULL; // "STLCKPT\x01"
    static constexpr uint64_t CKPT_VERSION = 1;

    static uint32_t crc32(const void* data, size_t len);
};

} // namespace straylight::pmem
