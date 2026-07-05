// bin/pmem/dax.h
// DAX device management for persistent memory
#pragma once

#include <straylight/result.h>

#include <cstddef>
#include <string>
#include <unordered_map>

namespace straylight::pmem {

class DaxManager {
public:
    ~DaxManager();

    /// Memory-map a DAX device or file for persistent memory access.
    Result<void*, std::string> map_region(const std::string& dax_path, size_t size);

    /// Unmap a previously mapped region.
    Result<void, std::string> unmap(void* addr, size_t size);

    /// Flush a range to persistence (CLWB/CLFLUSHOPT on real pmem, msync on files).
    Result<void, std::string> flush(void* addr, size_t size);

    /// Check if a path is a real pmem DAX device.
    bool is_pmem(const std::string& path) const;

private:
    struct MappedRegion {
        std::string path;
        size_t size;
        bool is_pmem;
    };
    std::unordered_map<void*, MappedRegion> mappings_;
};

} // namespace straylight::pmem
