// bin/enclave/sealed_storage.h
// SGX sealed storage — AES-256-GCM encryption bound to enclave identity.
// Hardware mode uses sgx_get_key(); stub mode uses a deterministic fixed key.
#pragma once

#include "attestation.h"

#include <straylight/result.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace straylight::enclave {

/// Policy controlling which enclaves may unseal a blob.
enum class SealPolicy : uint8_t {
    MrEnclave = 0, ///< Only this exact enclave binary (MRENCLAVE match).
    MrSigner  = 1, ///< Any enclave signed by the same key (MRSIGNER match).
};

/// An encrypted+authenticated blob produced by SealedStore::seal().
struct SealedBlob {
    std::vector<uint8_t> ciphertext; ///< AES-256-GCM ciphertext (same length as plaintext)
    std::vector<uint8_t> tag;        ///< 16-byte GCM authentication tag
    std::vector<uint8_t> nonce;      ///< 12-byte GCM nonce (random per seal operation)
    SealPolicy           policy{SealPolicy::MrEnclave};
};

/// Sealed storage manager.
/// Create one instance per enclave context; call init() before seal/unseal.
class SealedStore {
public:
    /// Initialise with the given mode.
    /// Derives the enclave sealing key (EGETKEY in hardware, fixed key in stub).
    Result<void, std::string> init(SgxMode mode);

    /// Seal plaintext bytes using AES-256-GCM.
    /// @param plaintext Byte span to encrypt. Empty spans are allowed.
    /// @param policy    Seal policy embedded in the blob.
    /// @returns SealedBlob or error string.
    Result<SealedBlob, std::string>
    seal(std::span<const uint8_t> plaintext, SealPolicy policy);

    /// Unseal a previously sealed blob.
    /// @returns Plaintext bytes, or error string if tag verification fails.
    Result<std::vector<uint8_t>, std::string>
    unseal(const SealedBlob& blob);

    /// Returns true if init() has been called successfully.
    [[nodiscard]] bool initialized() const { return initialized_; }

private:
    SgxMode              mode_{SgxMode::Stub};
    bool                 initialized_{false};
    std::vector<uint8_t> enclave_key_; ///< 32-byte AES-256 key

    /// Derive a deterministic stub key (all bytes 0xAB).
    void derive_stub_key();

    /// Derive hardware sealing key via sgx_get_key() (only available with SGX SDK).
    Result<void, std::string> derive_hw_key(SealPolicy policy);
};

} // namespace straylight::enclave
