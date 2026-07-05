#pragma once

#include <straylight/export.h>
#include <straylight/result.h>

#include <cstddef>
#include <string>
#include <vector>

namespace straylight::hw {

/// Intel SGX enclave abstraction.
/// Implementation deferred to Plan 7 (Exotic Subsystems). Requires Intel SGX SDK.
class STRAYLIGHT_EXPORT SgxEnclave {
public:
    /// Create and initialize an enclave from a signed .so.
    static Result<SgxEnclave, std::string> create(const std::string& enclave_path);

    /// Call an ECALL (enclave function).
    Result<std::vector<uint8_t>, std::string> ecall(
        uint32_t function_id, const void* input, size_t input_len);

    /// Destroy the enclave.
    void destroy();

    /// Check if SGX is available on this hardware.
    static bool is_available();

    ~SgxEnclave();
    SgxEnclave(SgxEnclave&&) noexcept;
    SgxEnclave& operator=(SgxEnclave&&) noexcept;

private:
    SgxEnclave() = default;
    uint64_t enclave_id_ = 0;
};

} // namespace straylight::hw
