#pragma once

#include <straylight/result.h>
#include <straylight/error.h>

#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace straylight {

struct DrbgStats {
    std::string name = "primary";
    std::string algorithm = "ctr-drbg-aes-256";
    bool seeded = false;
    uint64_t bytes_generated = 0;
    uint64_t generate_calls = 0;
    uint64_t reseed_count = 0;
    bool health_ok = true;
    std::string last_error;
};

/// CTR-DRBG-style generator using AES-256 as the block cipher.
/// This preserves deterministic DRBG semantics while using OpenSSL EVP for the
/// block operation instead of the old XOR/counter placeholder.
class CtrDrbg {
public:
    Result<void, SLError> seed(const std::array<uint8_t, 32>& entropy);
    Result<void, SLError> reseed(const std::array<uint8_t, 32>& additional);
    Result<std::vector<uint8_t>, SLError> generate(size_t n_bytes);
    DrbgStats stats() const;

private:
    std::array<uint8_t, 32> key_{};
    std::array<uint8_t, 16> counter_{};
    bool seeded_ = false;
    mutable std::mutex mutex_;
    uint64_t bytes_generated_ = 0;
    uint64_t generate_calls_ = 0;
    uint64_t reseed_count_ = 0;
    bool health_ok_ = true;
    std::string last_error_;

    Result<void, SLError> update(const std::array<uint8_t, 48>& provided_data);
    void increment_counter();
    void record_error(const SLError& err);
};

} // namespace straylight
