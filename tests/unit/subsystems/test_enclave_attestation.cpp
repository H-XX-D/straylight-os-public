// tests/unit/subsystems/test_enclave_attestation.cpp
// Unit tests for straylight-enclave: AttestationCtx and SealedStore.
// All tests run in SgxMode::Stub so no SGX hardware is required.

#include <gtest/gtest.h>

#include "attestation.h"
#include "sealed_storage.h"

#include <cstdint>
#include <vector>

using namespace straylight::enclave;

// ============================================================================
// AttestationCtx tests
// ============================================================================

TEST(Attestation, InitStubSucceeds) {
    AttestationCtx ctx;
    auto r = ctx.init(SgxMode::Stub);
    ASSERT_TRUE(r.has_value()) << r.error();
}

TEST(Attestation, LocalReportGenerates) {
    AttestationCtx ctx;
    auto r = ctx.init(SgxMode::Stub);
    ASSERT_TRUE(r.has_value());

    auto report = ctx.generate_local_report();
    ASSERT_TRUE(report.has_value()) << report.error();

    // MRENCLAVE and MRSIGNER must be non-empty hex strings.
    EXPECT_FALSE(report->mr_enclave.empty());
    EXPECT_FALSE(report->mr_signer.empty());

    // ISV SVN must be at least 1.
    EXPECT_GT(report->isv_svn, static_cast<uint16_t>(0));

    // Report data must be 64 bytes.
    EXPECT_EQ(report->report_data.size(), 64u);
}

TEST(Attestation, LocalReportIsNotInitializedFails) {
    AttestationCtx ctx;
    // No init() call.
    auto r = ctx.generate_local_report();
    EXPECT_FALSE(r.has_value());
}

TEST(Attestation, RemoteQuoteFromReport) {
    AttestationCtx ctx;
    ctx.init(SgxMode::Stub);

    auto report = ctx.generate_local_report();
    ASSERT_TRUE(report.has_value());

    auto quote = ctx.generate_remote_quote(*report);
    ASSERT_TRUE(quote.has_value()) << quote.error();

    // Quote data must be non-empty.
    EXPECT_FALSE(quote->data.empty());
    // EPID group ID must be set.
    EXPECT_FALSE(quote->epid_group_id.empty());
}

TEST(Attestation, VerifyQuoteRoundtrip) {
    AttestationCtx ctx;
    ctx.init(SgxMode::Stub);

    auto report = ctx.generate_local_report();
    ASSERT_TRUE(report.has_value());

    auto quote = ctx.generate_remote_quote(*report);
    ASSERT_TRUE(quote.has_value());

    auto ok = ctx.verify_quote(*quote);
    ASSERT_TRUE(ok.has_value()) << ok.error();
    EXPECT_TRUE(*ok);
}

TEST(Attestation, TamperedQuoteFailsVerification) {
    AttestationCtx ctx;
    ctx.init(SgxMode::Stub);

    auto report = ctx.generate_local_report();
    auto quote  = ctx.generate_remote_quote(*report);
    ASSERT_TRUE(quote.has_value());

    // Corrupt the sentinel byte.
    RemoteQuote bad = *quote;
    if (!bad.data.empty()) {
        bad.data.back() ^= 0xFF;
    }
    auto ok = ctx.verify_quote(bad);
    ASSERT_TRUE(ok.has_value());
    EXPECT_FALSE(*ok);
}

TEST(Attestation, MultipleReportsAreDeterministic) {
    AttestationCtx ctx;
    ctx.init(SgxMode::Stub);

    auto r1 = ctx.generate_local_report();
    auto r2 = ctx.generate_local_report();
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());

    // Stub always produces the same measurement.
    EXPECT_EQ(r1->mr_enclave, r2->mr_enclave);
    EXPECT_EQ(r1->mr_signer,  r2->mr_signer);
}

// ============================================================================
// SealedStore tests
// ============================================================================

TEST(SealedStorage, InitStubSucceeds) {
    SealedStore store;
    auto r = store.init(SgxMode::Stub);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_TRUE(store.initialized());
}

TEST(SealedStorage, SealUnsealRoundtrip) {
    SealedStore store;
    store.init(SgxMode::Stub);

    const std::vector<uint8_t> plaintext = {0xDE, 0xAD, 0xBE, 0xEF};

    auto sealed = store.seal(plaintext, SealPolicy::MrEnclave);
    ASSERT_TRUE(sealed.has_value()) << sealed.error();

    // Ciphertext must differ from plaintext (AES-GCM is not identity).
    EXPECT_NE(sealed->ciphertext, plaintext);

    // Nonce must be 12 bytes, tag must be 16 bytes.
    EXPECT_EQ(sealed->nonce.size(), 12u);
    EXPECT_EQ(sealed->tag.size(),   16u);

    auto unsealed = store.unseal(*sealed);
    ASSERT_TRUE(unsealed.has_value()) << unsealed.error();
    EXPECT_EQ(*unsealed, plaintext);
}

TEST(SealedStorage, EmptyPlaintextRoundtrip) {
    SealedStore store;
    store.init(SgxMode::Stub);

    const std::vector<uint8_t> empty;
    auto sealed = store.seal(empty, SealPolicy::MrSigner);
    ASSERT_TRUE(sealed.has_value()) << sealed.error();
    EXPECT_TRUE(sealed->ciphertext.empty());

    auto unsealed = store.unseal(*sealed);
    ASSERT_TRUE(unsealed.has_value()) << unsealed.error();
    EXPECT_TRUE(unsealed->empty());
}

TEST(SealedStorage, LargePayloadRoundtrip) {
    SealedStore store;
    store.init(SgxMode::Stub);

    // 1 MiB of pattern data.
    std::vector<uint8_t> large(1024 * 1024);
    for (size_t i = 0; i < large.size(); ++i) {
        large[i] = static_cast<uint8_t>(i & 0xFF);
    }

    auto sealed = store.seal(large, SealPolicy::MrEnclave);
    ASSERT_TRUE(sealed.has_value()) << sealed.error();

    auto unsealed = store.unseal(*sealed);
    ASSERT_TRUE(unsealed.has_value()) << unsealed.error();
    EXPECT_EQ(*unsealed, large);
}

TEST(SealedStorage, TamperedTagFails) {
    SealedStore store;
    store.init(SgxMode::Stub);

    const std::vector<uint8_t> data = {1, 2, 3, 4};
    auto sealed = store.seal(data, SealPolicy::MrEnclave);
    ASSERT_TRUE(sealed.has_value());

    // Corrupt the authentication tag.
    SealedBlob bad = *sealed;
    bad.tag[0] ^= 0xFF;

    auto result = store.unseal(bad);
    EXPECT_FALSE(result.has_value());
}

TEST(SealedStorage, TamperedCiphertextFails) {
    SealedStore store;
    store.init(SgxMode::Stub);

    const std::vector<uint8_t> data = {0xAA, 0xBB, 0xCC, 0xDD};
    auto sealed = store.seal(data, SealPolicy::MrEnclave);
    ASSERT_TRUE(sealed.has_value());

    SealedBlob bad = *sealed;
    if (!bad.ciphertext.empty()) {
        bad.ciphertext[0] ^= 0x01;
    }

    auto result = store.unseal(bad);
    EXPECT_FALSE(result.has_value());
}

TEST(SealedStorage, UniqueNoncePerSeal) {
    SealedStore store;
    store.init(SgxMode::Stub);

    const std::vector<uint8_t> data = {1, 2, 3};
    auto b1 = store.seal(data, SealPolicy::MrEnclave);
    auto b2 = store.seal(data, SealPolicy::MrEnclave);
    ASSERT_TRUE(b1.has_value());
    ASSERT_TRUE(b2.has_value());

    // Each seal call must generate a fresh random nonce.
    EXPECT_NE(b1->nonce, b2->nonce);
}

TEST(SealedStorage, UnsealWithoutInitFails) {
    SealedStore store; // Not initialised.
    SealedBlob blob;
    blob.nonce.resize(12, 0);
    blob.tag.resize(16, 0);
    auto r = store.unseal(blob);
    EXPECT_FALSE(r.has_value());
}
