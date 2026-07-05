#pragma once

#include <straylight/export.h>
#include <straylight/result.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace straylight::hw {

/// Hardware entropy source. Uses RDRAND/RDSEED when available, falls back to /dev/urandom.
class STRAYLIGHT_EXPORT EntropySource {
public:
    EntropySource();
    ~EntropySource();

    /// Fill buffer with random bytes.
    straylight::Result<void, std::string> fill(void* buf, size_t len);

    /// Generate a random 64-bit integer.
    straylight::Result<uint64_t, std::string> random_u64();

    /// Run basic health check on the entropy source.
    straylight::Result<void, std::string> health_check();

    /// Whether hardware RNG (RDRAND) is available.
    [[nodiscard]] bool has_hardware_rng() const noexcept { return has_rdrand_; }

private:
    bool has_rdrand_ = false;
    int urandom_fd_ = -1;
};

} // namespace straylight::hw
