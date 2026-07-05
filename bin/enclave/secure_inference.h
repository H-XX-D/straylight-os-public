// bin/enclave/secure_inference.h
// Secure ML inference: load sealed model, decrypt inside enclave boundary,
// execute forward pass, re-seal output before returning.
#pragma once

#include "attestation.h"
#include "sealed_storage.h"

#include <straylight/config.h>
#include <straylight/result.h>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace straylight::enclave {

/// Input bundle for a single inference call.
struct InferenceRequest {
    std::filesystem::path encrypted_model; ///< Path to sealed model file on disk
    std::vector<uint8_t>  input_tensor;    ///< Raw input bytes (caller serialises)
};

/// Output bundle returned by SecureInference::run().
struct InferenceResult {
    std::vector<uint8_t> encrypted_output; ///< Re-sealed output bytes
    uint64_t             latency_us{0};    ///< Wall-clock microseconds for the call
};

/// Secure inference engine.
/// Loads and decrypts models inside the enclave boundary (or stub simulation).
class SecureInference {
public:
    /// Initialise the engine. Must be called before run().
    Result<void, std::string> init(SgxMode mode, const straylight::Config& cfg);

    /// Run one inference request end-to-end.
    Result<InferenceResult, std::string> run(const InferenceRequest& req);

    /// Release any held model state.
    void teardown();

private:
    SealedStore sealer_;
    SgxMode     mode_{SgxMode::Stub};
    bool        initialized_{false};

    /// Read and unseal a model file.
    /// On-disk format: [nonce:12][tag:16][ciphertext:N].
    Result<std::vector<uint8_t>, std::string>
    load_and_decrypt_model(const std::filesystem::path& path);

    /// Execute the computation graph on decrypted model data and raw input.
    /// Stub: XORs each input byte with model[0] to produce output.
    Result<std::vector<uint8_t>, std::string>
    execute_graph(std::span<const uint8_t> model,
                  std::span<const uint8_t> input);
};

} // namespace straylight::enclave
