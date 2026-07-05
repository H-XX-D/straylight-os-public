// bin/enclave/attestation.h
// SGX local/remote attestation — hardware or software-stub fallback
#pragma once

#include <straylight/result.h>

#include <cstdint>
#include <string>
#include <vector>

namespace straylight::enclave {

/// Operating mode for SGX operations.
enum class SgxMode : uint8_t {
    Hardware, ///< Real SGX hardware via /dev/sgx_enclave
    Stub,     ///< Deterministic software simulation for testing
};

/// Local attestation report produced by EREPORT (or simulated equivalent).
struct LocalReport {
    std::string          mr_enclave;   ///< Hex-encoded SHA-256 enclave measurement
    std::string          mr_signer;    ///< Hex-encoded SHA-256 signing-key measurement
    uint16_t             isv_svn{0};   ///< ISV security version number
    std::vector<uint8_t> report_data;  ///< Up to 64 bytes of user-supplied binding data
};

/// Remote attestation quote wrapping a signed report (EPID/DCAP).
struct RemoteQuote {
    std::vector<uint8_t> data;          ///< Serialised quote bytes
    std::string          epid_group_id; ///< EPID GID or "dcap" for ECDSA
};

/// Context for SGX attestation operations.
/// Stateful: call init() once before generate/verify methods.
class AttestationCtx {
public:
    /// Initialise context for the given mode.
    /// Hardware mode checks /dev/sgx_enclave is accessible.
    Result<void, std::string> init(SgxMode mode);

    /// Generate a local attestation report.
    /// @returns LocalReport or error string.
    Result<LocalReport, std::string> generate_local_report();

    /// Generate a remote attestation quote from a local report.
    Result<RemoteQuote, std::string> generate_remote_quote(const LocalReport& report);

    /// Verify a remote quote's integrity.
    /// @returns true if the quote is structurally valid and signature checks pass.
    Result<bool, std::string> verify_quote(const RemoteQuote& quote);

private:
    SgxMode mode_{SgxMode::Stub};
    bool    initialized_{false};

    // Hardware paths wrap sgx_create_report / sgx_init_quote / sgx_get_quote.
    Result<LocalReport, std::string> hw_report();
    Result<RemoteQuote, std::string> hw_quote(const LocalReport& r);

    // Stub paths produce deterministic values suitable for unit tests.
    Result<LocalReport, std::string> stub_report();
    Result<RemoteQuote, std::string> stub_quote(const LocalReport& r);
};

} // namespace straylight::enclave
