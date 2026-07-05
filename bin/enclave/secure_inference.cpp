// bin/enclave/secure_inference.cpp
// Secure inference engine — model is loaded from a sealed file, decrypted,
// executed as a simple linear graph, and the output is re-sealed.

#include "secure_inference.h"

#include <straylight/log.h>

#include <chrono>
#include <fstream>
#include <iterator>

namespace straylight::enclave {

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

Result<void, std::string>
SecureInference::init(SgxMode mode, const straylight::Config& /*cfg*/) {
    mode_ = mode;
    auto r = sealer_.init(mode);
    if (!r.has_value()) {
        return r;
    }
    initialized_ = true;
    SL_DEBUG("SecureInference initialized mode={}",
             mode == SgxMode::Hardware ? "hardware" : "stub");
    return Result<void, std::string>::ok();
}

Result<InferenceResult, std::string>
SecureInference::run(const InferenceRequest& req) {
    if (!initialized_) {
        return Result<InferenceResult, std::string>::error(
            "SecureInference not initialized — call init() first");
    }

    auto t0 = std::chrono::steady_clock::now();

    // Step 1: load and decrypt the model from disk.
    auto model = load_and_decrypt_model(req.encrypted_model);
    if (!model.has_value()) {
        return Result<InferenceResult, std::string>::error(
            "Model load failed: " + model.error());
    }

    // Step 2: execute the computation graph.
    auto output = execute_graph(model.value(), req.input_tensor);
    if (!output.has_value()) {
        return Result<InferenceResult, std::string>::error(
            "Graph execution failed: " + output.error());
    }

    // Step 3: re-seal the output before it leaves the enclave boundary.
    auto sealed = sealer_.seal(output.value(), SealPolicy::MrEnclave);
    if (!sealed.has_value()) {
        return Result<InferenceResult, std::string>::error(
            "Output sealing failed: " + sealed.error());
    }

    auto dt = std::chrono::steady_clock::now() - t0;
    InferenceResult result{
        .encrypted_output = std::move(sealed.value().ciphertext),
        .latency_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(dt).count()),
    };
    SL_DEBUG("SecureInference::run latency={}us output_bytes={}",
             result.latency_us, result.encrypted_output.size());
    return Result<InferenceResult, std::string>::ok(std::move(result));
}

void SecureInference::teardown() {
    initialized_ = false;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

Result<std::vector<uint8_t>, std::string>
SecureInference::load_and_decrypt_model(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        return Result<std::vector<uint8_t>, std::string>::error(
            "Cannot open model file: " + path.string());
    }

    std::vector<uint8_t> raw(std::istreambuf_iterator<char>(f),
                             std::istreambuf_iterator<char>{});

    // On-disk sealed model layout: [nonce:12][tag:16][ciphertext:N]
    constexpr size_t NONCE_SIZE = 12;
    constexpr size_t TAG_SIZE   = 16;
    constexpr size_t MIN_SIZE   = NONCE_SIZE + TAG_SIZE;

    if (raw.size() < MIN_SIZE) {
        return Result<std::vector<uint8_t>, std::string>::error(
            "Sealed model file too small: " + std::to_string(raw.size()) +
            " bytes (minimum " + std::to_string(MIN_SIZE) + ")");
    }

    SealedBlob blob;
    blob.nonce.assign(raw.begin(), raw.begin() + NONCE_SIZE);
    blob.tag.assign(raw.begin() + NONCE_SIZE, raw.begin() + MIN_SIZE);
    blob.ciphertext.assign(raw.begin() + MIN_SIZE, raw.end());
    blob.policy = SealPolicy::MrEnclave;

    return sealer_.unseal(blob);
}

Result<std::vector<uint8_t>, std::string>
SecureInference::execute_graph(std::span<const uint8_t> model,
                                std::span<const uint8_t> input) {
    if (input.empty()) {
        return Result<std::vector<uint8_t>, std::string>::error(
            "Input tensor is empty");
    }

    // In hardware mode this would dispatch via an ECALL into the trusted
    // compute graph inside the SGX enclave.  In stub mode we apply a simple
    // element-wise XOR of each input byte with model[0] — sufficient for
    // unit-test round-trip verification without a real ML runtime.
    std::vector<uint8_t> out(input.begin(), input.end());
    if (!model.empty()) {
        const uint8_t key = model[0];
        for (auto& b : out) {
            b = static_cast<uint8_t>(b ^ key);
        }
    }
    return Result<std::vector<uint8_t>, std::string>::ok(std::move(out));
}

} // namespace straylight::enclave
