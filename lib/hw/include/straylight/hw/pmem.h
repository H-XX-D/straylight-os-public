#pragma once

#include <straylight/export.h>
#include <straylight/result.h>

#include <cstddef>
#include <string>

namespace straylight::hw {

/// Persistent memory (Intel PMEM / CXL) abstraction.
/// Implementation deferred to Plan 7 (Exotic Subsystems). Requires libpmem2.
class STRAYLIGHT_EXPORT PmemRegion {
public:
    /// Map a DAX device or file to persistent memory.
    static Result<PmemRegion, std::string> open(const std::string& path);

    /// Get pointer to mapped region.
    [[nodiscard]] void* data() noexcept;
    [[nodiscard]] const void* data() const noexcept;
    [[nodiscard]] size_t size() const noexcept;

    /// Flush writes to persistence.
    void persist(void* addr, size_t len);

    ~PmemRegion();
    PmemRegion(PmemRegion&&) noexcept;
    PmemRegion& operator=(PmemRegion&&) noexcept;

private:
    PmemRegion() = default;
    void* data_ = nullptr;
    size_t size_ = 0;
};

} // namespace straylight::hw
