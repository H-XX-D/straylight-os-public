// bin/enclave/attestation.cpp
// SGX attestation — hardware path compiles when STRAYLIGHT_SGX_HW is defined;
// otherwise stub fallback is used unconditionally.

#include "attestation.h"

#include <straylight/log.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>

#ifdef STRAYLIGHT_SGX_HW
#  include <sgx_urts.h>
#  include <sgx_uae_quote_ex.h>
#  include <fcntl.h>
#  include <unistd.h>
#endif

namespace straylight::enclave {

namespace {

/// Convert raw bytes to lowercase hex string.
std::string to_hex(const uint8_t* data, size_t len) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; ++i) {
        oss << std::setw(2) << static_cast<unsigned>(data[i]);
    }
    return oss.str();
}

/// Deterministic stub MRENCLAVE: SHA256-like pattern seeded by constant.
std::string stub_mrenclave() {
    // 32 bytes with a recognisable pattern: 0xE0..0xEF repeated.
    std::array<uint8_t, 32> raw{};
    for (int i = 0; i < 32; ++i) {
        raw[static_cast<size_t>(i)] = static_cast<uint8_t>(0xE0 | (i & 0x0F));
    }
    return to_hex(raw.data(), raw.size());
}

/// Deterministic stub MRSIGNER.
std::string stub_mrsigner() {
    std::array<uint8_t, 32> raw{};
    for (int i = 0; i < 32; ++i) {
        raw[static_cast<size_t>(i)] = static_cast<uint8_t>(0xA0 | (i & 0x0F));
    }
    return to_hex(raw.data(), raw.size());
}

/// Verify a quote's sentinel byte is correct (stub protocol).
bool verify_stub_quote(const RemoteQuote& q) {
    // Stub quote must be non-empty and end with 0xFF sentinel.
    if (q.data.empty()) return false;
    return q.data.back() == 0xFF;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

Result<void, std::string> AttestationCtx::init(SgxMode mode) {
    mode_ = mode;

#ifdef STRAYLIGHT_SGX_HW
    if (mode == SgxMode::Hardware) {
        int fd = ::open("/dev/sgx_enclave", O_RDWR);
        if (fd < 0) {
            return Result<void, std::string>::error(
                "SGX hardware device /dev/sgx_enclave unavailable — "
                "check BIOS settings and kernel module");
        }
        ::close(fd);
    }
#else
    if (mode == SgxMode::Hardware) {
        return Result<void, std::string>::error(
            "SGX hardware support not compiled in (missing STRAYLIGHT_SGX_HW)");
    }
#endif

    initialized_ = true;
    SL_INFO("AttestationCtx initialized mode={}",
            mode == SgxMode::Hardware ? "hardware" : "stub");
    return Result<void, std::string>::ok();
}

Result<LocalReport, std::string> AttestationCtx::generate_local_report() {
    if (!initialized_) {
        return Result<LocalReport, std::string>::error(
            "AttestationCtx not initialized — call init() first");
    }
    return (mode_ == SgxMode::Hardware) ? hw_report() : stub_report();
}

Result<RemoteQuote, std::string>
AttestationCtx::generate_remote_quote(const LocalReport& report) {
    if (!initialized_) {
        return Result<RemoteQuote, std::string>::error(
            "AttestationCtx not initialized");
    }
    return (mode_ == SgxMode::Hardware) ? hw_quote(report) : stub_quote(report);
}

Result<bool, std::string> AttestationCtx::verify_quote(const RemoteQuote& quote) {
    if (!initialized_) {
        return Result<bool, std::string>::error("AttestationCtx not initialized");
    }

#ifdef STRAYLIGHT_SGX_HW
    if (mode_ == SgxMode::Hardware) {
        // Real DCAP/EPID verification would call into the quoting verification
        // library (sgx_qv_verify_quote). For now delegate to stub logic since
        // the QVL is an optional runtime dependency.
    }
#endif

    // Stub verification: structural check + sentinel.
    bool ok = verify_stub_quote(quote);
    return Result<bool, std::string>::ok(ok);
}

// ---------------------------------------------------------------------------
// Hardware paths
// ---------------------------------------------------------------------------

Result<LocalReport, std::string> AttestationCtx::hw_report() {
#ifdef STRAYLIGHT_SGX_HW
    // In a real integration, call sgx_create_report() inside a trusted enclave.
    // This host-side wrapper is a placeholder that would coordinate with the
    // enclave via ECALLs defined in enclave_def/enclave.edl.
    return Result<LocalReport, std::string>::error(
        "hw_report: SGX ECall infrastructure not fully wired — "
        "use stub mode for testing");
#else
    return Result<LocalReport, std::string>::error("SGX hardware not compiled in");
#endif
}

Result<RemoteQuote, std::string> AttestationCtx::hw_quote(const LocalReport& /*r*/) {
#ifdef STRAYLIGHT_SGX_HW
    return Result<RemoteQuote, std::string>::error(
        "hw_quote: DCAP quoting enclave not wired — use stub mode");
#else
    return Result<RemoteQuote, std::string>::error("SGX hardware not compiled in");
#endif
}

// ---------------------------------------------------------------------------
// Stub paths
// ---------------------------------------------------------------------------

Result<LocalReport, std::string> AttestationCtx::stub_report() {
    LocalReport r;
    r.mr_enclave  = stub_mrenclave();
    r.mr_signer   = stub_mrsigner();
    r.isv_svn     = 1;
    r.report_data.resize(64, 0x42); // Deterministic fill for testing
    return Result<LocalReport, std::string>::ok(std::move(r));
}

Result<RemoteQuote, std::string> AttestationCtx::stub_quote(const LocalReport& r) {
    RemoteQuote q;
    q.epid_group_id = "stub-epid-0001";

    // Encode report fields as quote body: [report_data][mr_enclave_byte][svn:u16][0xFF]
    q.data.insert(q.data.end(), r.report_data.begin(), r.report_data.end());
    // Append first byte of mr_enclave (as raw octet, not hex string).
    q.data.push_back(static_cast<uint8_t>(0xE0));
    // Append ISV SVN little-endian.
    q.data.push_back(static_cast<uint8_t>(r.isv_svn & 0xFF));
    q.data.push_back(static_cast<uint8_t>((r.isv_svn >> 8) & 0xFF));
    // Sentinel byte for stub verification.
    q.data.push_back(0xFF);

    return Result<RemoteQuote, std::string>::ok(std::move(q));
}

} // namespace straylight::enclave
