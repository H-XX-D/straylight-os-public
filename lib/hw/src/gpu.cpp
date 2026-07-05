#include <straylight/hw/gpu.h>
#include <straylight/log.h>

#include <cstdlib>

namespace straylight::hw {

GpuAllocator::GpuAllocator(GpuBackend backend) : backend_(backend) {
    SL_DEBUG("GpuAllocator created with backend {}", static_cast<int>(backend));
}

GpuAllocator::~GpuAllocator() {
    std::lock_guard lock(mu_);
    for (auto& [ptr, size] : allocs_) {
        // All backends currently use aligned_alloc; route through std::free.
        // When real GPU backends are added, this must be updated.
        std::free(ptr);
    }
}

straylight::Result<void*, std::string> GpuAllocator::allocate(size_t bytes) {
    std::lock_guard lock(mu_);

    if (bytes == 0) {
        return straylight::Result<void*, std::string>::error("Cannot allocate 0 bytes");
    }

    // aligned_alloc requires size to be a multiple of alignment
    constexpr size_t alignment = 64;  // 64-byte aligned for SIMD/cache lines
    size_t aligned_bytes = (bytes + alignment - 1) & ~(alignment - 1);

    void* ptr = nullptr;
    switch (backend_) {
        case GpuBackend::CPU:
            ptr = std::aligned_alloc(alignment, aligned_bytes);
            break;
        case GpuBackend::CUDA:
        case GpuBackend::ROCm:
        case GpuBackend::OneAPI:
            // TODO: real GPU allocation via cuMemAlloc/hipMalloc/zeMemAllocDevice
            ptr = std::aligned_alloc(alignment, aligned_bytes);
            break;
    }

    if (!ptr) {
        return straylight::Result<void*, std::string>::error("Allocation failed");
    }

    allocs_[ptr] = bytes;
    stats_.allocations++;
    stats_.bytes_allocated += bytes;
    if (stats_.bytes_allocated > stats_.peak_bytes) {
        stats_.peak_bytes = stats_.bytes_allocated;
    }

    return straylight::Result<void*, std::string>::ok(ptr);
}

void GpuAllocator::free(void* ptr) {
    std::lock_guard lock(mu_);
    auto it = allocs_.find(ptr);
    if (it == allocs_.end()) return;

    stats_.bytes_allocated -= it->second;
    allocs_.erase(it);

    // All backends currently use aligned_alloc; route through std::free.
    std::free(ptr);
}

AllocStats GpuAllocator::stats() const {
    std::lock_guard lock(mu_);
    return stats_;
}

} // namespace straylight::hw
