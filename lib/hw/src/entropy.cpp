#include <straylight/hw/entropy.h>
#include <straylight/log.h>

#include <fcntl.h>
#include <unistd.h>
#include <cstring>

#ifdef __x86_64__
#include <immintrin.h>
#include <cpuid.h>
#endif

namespace straylight::hw {

EntropySource::EntropySource() {
#ifdef __x86_64__
    unsigned int eax, ebx, ecx, edx;
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        has_rdrand_ = (ecx >> 30) & 1;
    }
#endif
    urandom_fd_ = ::open("/dev/urandom", O_RDONLY);
    SL_DEBUG("EntropySource: RDRAND={}, urandom_fd={}", has_rdrand_, urandom_fd_);
}

EntropySource::~EntropySource() {
    if (urandom_fd_ >= 0) ::close(urandom_fd_);
}

straylight::Result<void, std::string> EntropySource::fill(void* buf, size_t len) {
    auto* dst = static_cast<uint8_t*>(buf);
    size_t filled = 0;

#ifdef __x86_64__
    if (has_rdrand_ && len <= 4096) {
        while (filled + 8 <= len) {
            unsigned long long val;
            if (_rdrand64_step(&val)) {
                std::memcpy(dst + filled, &val, 8);
                filled += 8;
            } else {
                break;  // Fall through to urandom for remainder
            }
        }
        // Handle trailing 1-7 bytes
        if (filled < len && filled + 8 > len) {
            unsigned long long val;
            if (_rdrand64_step(&val)) {
                std::memcpy(dst + filled, &val, len - filled);
                filled = len;
            }
        }
        if (filled == len) {
            return straylight::Result<void, std::string>::ok();
        }
    }
#endif

    if (urandom_fd_ < 0) {
        return straylight::Result<void, std::string>::error("/dev/urandom not available");
    }

    // Fill only the remaining unfilled bytes from urandom
    while (filled < len) {
        auto n = ::read(urandom_fd_, dst + filled, len - filled);
        if (n <= 0) {
            return straylight::Result<void, std::string>::error("read(/dev/urandom) failed");
        }
        filled += static_cast<size_t>(n);
    }

    return straylight::Result<void, std::string>::ok();
}

straylight::Result<uint64_t, std::string> EntropySource::random_u64() {
    uint64_t val = 0;
    auto r = fill(&val, sizeof(val));
    if (!r.has_value()) return straylight::Result<uint64_t, std::string>::error(r.error());
    return straylight::Result<uint64_t, std::string>::ok(val);
}

straylight::Result<void, std::string> EntropySource::health_check() {
    // Basic health: generate 256 bytes, verify not all zeros or all ones
    uint8_t buf[256];
    auto r = fill(buf, sizeof(buf));
    if (!r.has_value()) return r;

    int ones = 0;
    for (auto b : buf) {
        for (int i = 0; i < 8; i++) ones += (b >> i) & 1;
    }

    // Monobit test: expect ~1024 ones out of 2048 bits
    // Accept if between 800 and 1248 (very loose bounds)
    if (ones < 800 || ones > 1248) {
        return straylight::Result<void, std::string>::error(
            "Entropy health check failed: monobit test (" + std::to_string(ones) + "/2048)");
    }

    return straylight::Result<void, std::string>::ok();
}

} // namespace straylight::hw
