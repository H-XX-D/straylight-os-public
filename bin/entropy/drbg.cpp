#include "drbg.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>

#include <algorithm>
#include <cstring>

namespace straylight {
namespace {

constexpr size_t kKeyBytes = 32;
constexpr size_t kBlockBytes = 16;
constexpr size_t kSeedMaterialBytes = kKeyBytes + kBlockBytes;

template <size_t N>
void secure_clear(std::array<uint8_t, N>& buf) {
    OPENSSL_cleanse(buf.data(), buf.size());
}

void secure_clear(std::vector<uint8_t>& buf) {
    if (!buf.empty()) {
        OPENSSL_cleanse(buf.data(), buf.size());
    }
}

Result<std::array<uint8_t, kBlockBytes>, SLError> encrypt_block(
        const std::array<uint8_t, kKeyBytes>& key,
        const std::array<uint8_t, kBlockBytes>& block) {
    std::array<uint8_t, kBlockBytes> out{};

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        return Result<std::array<uint8_t, kBlockBytes>, SLError>::error(
            SLError{SLErrorCode::Internal, "OpenSSL EVP_CIPHER_CTX_new failed"});
    }

    auto fail = [&](const char* message) {
        EVP_CIPHER_CTX_free(ctx);
        secure_clear(out);
        return Result<std::array<uint8_t, kBlockBytes>, SLError>::error(
            SLError{SLErrorCode::Internal, message});
    };

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_ecb(), nullptr, key.data(), nullptr) != 1) {
        return fail("OpenSSL EVP_EncryptInit_ex failed");
    }
    if (EVP_CIPHER_CTX_set_padding(ctx, 0) != 1) {
        return fail("OpenSSL EVP_CIPHER_CTX_set_padding failed");
    }

    int len = 0;
    if (EVP_EncryptUpdate(ctx, out.data(), &len, block.data(), block.size()) != 1 ||
        len != static_cast<int>(out.size())) {
        return fail("OpenSSL EVP_EncryptUpdate failed");
    }

    std::array<uint8_t, kBlockBytes> final_buf{};
    int final_len = 0;
    if (EVP_EncryptFinal_ex(ctx, final_buf.data(), &final_len) != 1 || final_len != 0) {
        secure_clear(final_buf);
        return fail("OpenSSL EVP_EncryptFinal_ex failed");
    }
    secure_clear(final_buf);

    EVP_CIPHER_CTX_free(ctx);
    return Result<std::array<uint8_t, kBlockBytes>, SLError>::ok(out);
}

std::array<uint8_t, kSeedMaterialBytes> seed_material_from_32(
        const std::array<uint8_t, kKeyBytes>& input) {
    std::array<uint8_t, kSeedMaterialBytes> material{};
    std::copy(input.begin(), input.end(), material.begin());
    return material;
}

} // namespace

void CtrDrbg::increment_counter() {
    // Big-endian increment of 128-bit counter.
    for (int i = static_cast<int>(counter_.size()) - 1; i >= 0; --i) {
        if (++counter_[static_cast<size_t>(i)] != 0) break;
    }
}

void CtrDrbg::record_error(const SLError& err) {
    health_ok_ = false;
    last_error_ = err.message();
}

Result<void, SLError> CtrDrbg::update(
        const std::array<uint8_t, kSeedMaterialBytes>& provided_data) {
    std::array<uint8_t, kSeedMaterialBytes> temp{};
    size_t offset = 0;

    while (offset < temp.size()) {
        increment_counter();
        auto block = encrypt_block(key_, counter_);
        if (!block.has_value()) {
            record_error(block.error());
            secure_clear(temp);
            return Result<void, SLError>::error(block.error());
        }

        const size_t take = std::min(block.value().size(), temp.size() - offset);
        std::copy_n(block.value().begin(), take, temp.begin() + static_cast<long>(offset));
        offset += take;
    }

    for (size_t i = 0; i < temp.size(); ++i) {
        temp[i] ^= provided_data[i];
    }

    std::copy_n(temp.begin(), key_.size(), key_.begin());
    std::copy_n(temp.begin() + static_cast<long>(key_.size()),
                counter_.size(),
                counter_.begin());
    secure_clear(temp);
    return Result<void, SLError>::ok();
}

Result<void, SLError> CtrDrbg::seed(const std::array<uint8_t, 32>& entropy) {
    std::lock_guard lock(mutex_);

    key_.fill(0);
    counter_.fill(0);
    bytes_generated_ = 0;
    generate_calls_ = 0;
    reseed_count_ = 0;
    health_ok_ = true;
    last_error_.clear();

    auto material = seed_material_from_32(entropy);
    auto update_res = update(material);
    secure_clear(material);
    if (!update_res.has_value()) {
        seeded_ = false;
        return update_res;
    }

    seeded_ = true;
    return Result<void, SLError>::ok();
}

Result<void, SLError> CtrDrbg::reseed(const std::array<uint8_t, 32>& additional) {
    std::lock_guard lock(mutex_);
    if (!seeded_) {
        auto err = SLError{SLErrorCode::NotInitialized, "DRBG not seeded"};
        record_error(err);
        return Result<void, SLError>::error(err);
    }

    auto material = seed_material_from_32(additional);
    auto update_res = update(material);
    secure_clear(material);
    if (!update_res.has_value()) {
        return update_res;
    }

    ++reseed_count_;
    health_ok_ = true;
    last_error_.clear();
    return Result<void, SLError>::ok();
}

Result<std::vector<uint8_t>, SLError> CtrDrbg::generate(size_t n_bytes) {
    std::lock_guard lock(mutex_);
    if (!seeded_) {
        auto err = SLError{SLErrorCode::NotInitialized, "DRBG not seeded"};
        record_error(err);
        return Result<std::vector<uint8_t>, SLError>::error(err);
    }

    std::vector<uint8_t> output;
    output.reserve(n_bytes);

    while (output.size() < n_bytes) {
        increment_counter();
        auto block = encrypt_block(key_, counter_);
        if (!block.has_value()) {
            record_error(block.error());
            secure_clear(output);
            return Result<std::vector<uint8_t>, SLError>::error(block.error());
        }

        const size_t take = std::min(block.value().size(), n_bytes - output.size());
        output.insert(output.end(), block.value().begin(), block.value().begin() + static_cast<long>(take));
    }

    std::array<uint8_t, kSeedMaterialBytes> zero{};
    auto update_res = update(zero);
    secure_clear(zero);
    if (!update_res.has_value()) {
        secure_clear(output);
        return Result<std::vector<uint8_t>, SLError>::error(update_res.error());
    }

    bytes_generated_ += static_cast<uint64_t>(n_bytes);
    ++generate_calls_;
    health_ok_ = true;
    last_error_.clear();
    return Result<std::vector<uint8_t>, SLError>::ok(std::move(output));
}

DrbgStats CtrDrbg::stats() const {
    std::lock_guard lock(mutex_);
    DrbgStats s;
    s.seeded = seeded_;
    s.bytes_generated = bytes_generated_;
    s.generate_calls = generate_calls_;
    s.reseed_count = reseed_count_;
    s.health_ok = health_ok_;
    s.last_error = last_error_;
    return s;
}

} // namespace straylight
