// bin/enclave/enclave_def/enclave.cpp
// Trusted code that runs inside the SGX enclave.
// This file would be compiled with the SGX SDK toolchain into a signed .so.
// For StrayLight, we provide a software simulation that mirrors the real interface.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

// ============================================================================
// Simulated enclave state (in a real SGX build, these live in EPC memory)
// ============================================================================

namespace {

// Maximum model size: 64MB inside enclave.
static constexpr size_t MAX_MODEL_SIZE = 64 * 1024 * 1024;

// Simulated sealed key (MRSIGNER-derived in real SGX).
static constexpr uint8_t ENCLAVE_SEAL_KEY[16] = {
    0x53, 0x54, 0x4C, 0x45, 0x4E, 0x43, 0x4C, 0x41,
    0x56, 0x45, 0x4B, 0x45, 0x59, 0x30, 0x30, 0x31
};

struct EnclaveModel {
    std::vector<float> weights;
    uint32_t input_size = 0;
    uint32_t output_size = 0;
    bool loaded = false;
};

static EnclaveModel g_model;

// Simple AES-like XOR cipher for simulation (real SGX uses AES-GCM via EGETKEY).
void xor_cipher(uint8_t* data, size_t len, const uint8_t* key, size_t key_len) {
    for (size_t i = 0; i < len; ++i) {
        data[i] ^= key[i % key_len];
    }
}

// Simulated attestation report structure.
struct SimReport {
    uint8_t mr_enclave[32];   // Measurement of enclave code.
    uint8_t mr_signer[32];    // Measurement of signer key.
    uint8_t report_data[64];  // User-supplied data.
    uint64_t isv_prod_id;     // ISV product ID.
    uint16_t isv_svn;         // ISV security version number.
};

} // anonymous namespace

// ============================================================================
// ECALL implementations
// ============================================================================

extern "C" {

int ecall_load_model(const uint8_t* sealed_data, size_t sealed_size) {
    if (!sealed_data || sealed_size == 0) return -1;
    if (sealed_size > MAX_MODEL_SIZE) return -2;

    // Unseal: decrypt with enclave key.
    std::vector<uint8_t> decrypted(sealed_data, sealed_data + sealed_size);
    xor_cipher(decrypted.data(), decrypted.size(), ENCLAVE_SEAL_KEY, sizeof(ENCLAVE_SEAL_KEY));

    // Parse simple model format: [input_size:u32][output_size:u32][weights:float[]]
    if (decrypted.size() < 8) return -3;

    uint32_t in_sz, out_sz;
    std::memcpy(&in_sz, decrypted.data(), 4);
    std::memcpy(&out_sz, decrypted.data() + 4, 4);

    size_t expected_weight_bytes = static_cast<size_t>(in_sz) * out_sz * sizeof(float);
    if (decrypted.size() < 8 + expected_weight_bytes) return -4;

    g_model.input_size = in_sz;
    g_model.output_size = out_sz;
    g_model.weights.resize(static_cast<size_t>(in_sz) * out_sz);
    std::memcpy(g_model.weights.data(), decrypted.data() + 8, expected_weight_bytes);
    g_model.loaded = true;

    return 0;
}

int ecall_infer(const float* input, size_t input_len,
                float* output, size_t output_len) {
    if (!g_model.loaded) return -1;
    if (input_len != g_model.input_size) return -2;
    if (output_len < g_model.output_size) return -3;

    // Simple matrix-vector multiplication: output = weights^T * input + ReLU.
    for (uint32_t o = 0; o < g_model.output_size; ++o) {
        float sum = 0.0f;
        for (uint32_t i = 0; i < g_model.input_size; ++i) {
            sum += g_model.weights[i * g_model.output_size + o] * input[i];
        }
        // ReLU activation.
        output[o] = std::fmax(0.0f, sum);
    }

    return static_cast<int>(g_model.output_size);
}

int ecall_create_report(const uint8_t* user_data, size_t user_data_size,
                        uint8_t* report, size_t report_size) {
    if (report_size < sizeof(SimReport)) return -1;
    if (user_data_size > 64) return -2;

    SimReport rpt{};
    // Simulated MRENCLAVE: SHA256 of enclave binary (placeholder).
    for (int i = 0; i < 32; ++i) {
        rpt.mr_enclave[i] = static_cast<uint8_t>(0xE0 + (i & 0x0F));
    }
    // Simulated MRSIGNER.
    for (int i = 0; i < 32; ++i) {
        rpt.mr_signer[i] = static_cast<uint8_t>(0xA0 + (i & 0x0F));
    }
    // Copy user data.
    if (user_data && user_data_size > 0) {
        std::memcpy(rpt.report_data, user_data, user_data_size);
    }
    rpt.isv_prod_id = 1;
    rpt.isv_svn = 1;

    std::memcpy(report, &rpt, sizeof(rpt));
    return 0;
}

int ecall_seal(const uint8_t* plaintext, size_t plain_size,
               uint8_t* sealed, size_t sealed_buf_size,
               size_t* sealed_size) {
    if (!plaintext || plain_size == 0) return -1;
    if (sealed_buf_size < plain_size) return -2;

    std::memcpy(sealed, plaintext, plain_size);
    xor_cipher(sealed, plain_size, ENCLAVE_SEAL_KEY, sizeof(ENCLAVE_SEAL_KEY));
    *sealed_size = plain_size;
    return 0;
}

int ecall_unseal(const uint8_t* sealed, size_t sealed_size,
                 uint8_t* plaintext, size_t plain_buf_size,
                 size_t* plain_size) {
    if (!sealed || sealed_size == 0) return -1;
    if (plain_buf_size < sealed_size) return -2;

    std::memcpy(plaintext, sealed, sealed_size);
    xor_cipher(plaintext, sealed_size, ENCLAVE_SEAL_KEY, sizeof(ENCLAVE_SEAL_KEY));
    *plain_size = sealed_size;
    return 0;
}

} // extern "C"
