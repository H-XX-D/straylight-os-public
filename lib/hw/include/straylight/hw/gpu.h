#pragma once

#include <straylight/export.h>
#include <straylight/result.h>

#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>

namespace straylight::hw {

enum class GpuBackend : uint8_t {
    CPU = 0,    // Fallback: uses malloc
    CUDA = 1,
    ROCm = 2,
    OneAPI = 3,
};

struct AllocStats {
    size_t allocations = 0;
    size_t bytes_allocated = 0;
    size_t peak_bytes = 0;
};

/// Slab-style GPU memory allocator. Falls back to CPU malloc when no GPU.
class STRAYLIGHT_EXPORT GpuAllocator {
public:
    explicit GpuAllocator(GpuBackend backend = GpuBackend::CPU);
    ~GpuAllocator();

    /// Allocate bytes on the configured device.
    straylight::Result<void*, std::string> allocate(size_t bytes);

    /// Free a previously allocated pointer.
    void free(void* ptr);

    /// Current allocation statistics.
    [[nodiscard]] AllocStats stats() const;

    [[nodiscard]] GpuBackend backend() const noexcept { return backend_; }

private:
    GpuBackend backend_;
    mutable std::mutex mu_;
    std::unordered_map<void*, size_t> allocs_;
    AllocStats stats_;
};

} // namespace straylight::hw
