// tests/unit/subsystems/test_morph.cpp
#include <gtest/gtest.h>

#include "quantize.h"
#include "prune.h"
#include "adapt.h"

using namespace straylight::morph;

// ── Quantize tests ──────────────────────────────────────────────────────────

TEST(Quantize, Int8RoundTrip) {
    // Quantize and then dequantize, checking that values are approximately preserved
    std::vector<float> weights = {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f, 2.0f, -2.0f};

    QuantConfig cfg;
    cfg.mode = QuantMode::Int8;
    cfg.calibration_percentile = 100.0f;  // no clipping

    Quantizer quantizer;
    auto result = quantizer.quantize(weights, cfg);
    ASSERT_TRUE(result.has_value());

    const auto& qr = result.value();
    ASSERT_EQ(qr.quantized.size(), weights.size());
    EXPECT_EQ(qr.mode, QuantMode::Int8);
    EXPECT_GT(qr.scale, 0.0f);

    // Dequantize and check round-trip accuracy
    for (size_t i = 0; i < weights.size(); ++i) {
        float dequantized = (static_cast<float>(qr.quantized[i]) - static_cast<float>(qr.zero_point)) * qr.scale;
        EXPECT_NEAR(dequantized, weights[i], qr.scale * 1.5f)
            << "Mismatch at index " << i;
    }
}

TEST(Quantize, Int4Range) {
    std::vector<float> weights = {-1.0f, 0.0f, 1.0f};

    QuantConfig cfg;
    cfg.mode = QuantMode::Int4;

    Quantizer quantizer;
    auto result = quantizer.quantize(weights, cfg);
    ASSERT_TRUE(result.has_value());

    // Int4 range is [-8, 7]
    for (auto v : result.value().quantized) {
        EXPECT_GE(v, -8);
        EXPECT_LE(v, 7);
    }
}

TEST(Quantize, EmptyWeightsError) {
    Quantizer quantizer;
    auto result = quantizer.quantize({}, QuantConfig{});
    EXPECT_FALSE(result.has_value());
}

TEST(Quantize, InvalidPercentileError) {
    Quantizer quantizer;
    QuantConfig cfg;
    cfg.calibration_percentile = -1.0f;
    auto result = quantizer.quantize({1.0f}, cfg);
    EXPECT_FALSE(result.has_value());
}

TEST(Quantize, AllZeroWeights) {
    std::vector<float> weights(100, 0.0f);
    Quantizer quantizer;
    auto result = quantizer.quantize(weights, QuantConfig{});
    ASSERT_TRUE(result.has_value());
    // All quantized values should map to the zero point
    for (auto v : result.value().quantized) {
        EXPECT_EQ(v, static_cast<int8_t>(result.value().zero_point));
    }
}

// ── Prune tests ─────────────────────────────────────────────────────────────

TEST(Prune, MagnitudeSparsity) {
    std::vector<float> weights = {0.1f, -5.0f, 0.2f, -3.0f, 0.05f, 4.0f, 0.01f, -2.0f};

    PruneConfig cfg;
    cfg.strategy = PruneStrategy::Magnitude;
    cfg.sparsity = 0.5f;

    Pruner pruner;
    auto result = pruner.prune(weights, cfg);
    ASSERT_TRUE(result.has_value());

    const auto& pr = result.value();
    EXPECT_NEAR(pr.actual_sparsity, 0.5f, 0.15f);  // Allow some rounding tolerance

    // The largest-magnitude weights should be preserved
    EXPECT_NE(pr.pruned[1], 0.0f);  // -5.0 should survive
    EXPECT_NE(pr.pruned[5], 0.0f);  // 4.0 should survive

    // The smallest-magnitude weights should be zeroed
    EXPECT_EQ(pr.pruned[6], 0.0f);  // 0.01 should be pruned
}

TEST(Prune, StructuredPruning) {
    // 4 rows x 3 cols = 12 weights
    std::vector<float> weights = {
        0.1f, 0.1f, 0.1f,   // row 0: small norm
        5.0f, 5.0f, 5.0f,   // row 1: large norm
        0.2f, 0.2f, 0.2f,   // row 2: small norm
        3.0f, 3.0f, 3.0f    // row 3: medium norm
    };

    PruneConfig cfg;
    cfg.strategy = PruneStrategy::Structured;
    cfg.sparsity = 0.5f;
    cfg.structured_cols = 3;

    Pruner pruner;
    auto result = pruner.prune(weights, cfg);
    ASSERT_TRUE(result.has_value());

    const auto& pr = result.value();

    // Row 0 and row 2 should be pruned (smallest norms)
    // Entire row should be zero
    bool row0_zeroed = (pr.pruned[0] == 0.0f && pr.pruned[1] == 0.0f && pr.pruned[2] == 0.0f);
    bool row2_zeroed = (pr.pruned[6] == 0.0f && pr.pruned[7] == 0.0f && pr.pruned[8] == 0.0f);
    EXPECT_TRUE(row0_zeroed);
    EXPECT_TRUE(row2_zeroed);

    // Row 1 should be preserved
    EXPECT_NE(pr.pruned[3], 0.0f);
    EXPECT_NE(pr.pruned[4], 0.0f);
    EXPECT_NE(pr.pruned[5], 0.0f);
}

TEST(Prune, RandomPruning) {
    std::vector<float> weights(1000, 1.0f);

    PruneConfig cfg;
    cfg.strategy = PruneStrategy::Random;
    cfg.sparsity = 0.3f;
    cfg.seed = 12345;

    Pruner pruner;
    auto result = pruner.prune(weights, cfg);
    ASSERT_TRUE(result.has_value());

    EXPECT_NEAR(result.value().actual_sparsity, 0.3f, 0.05f);
}

TEST(Prune, EmptyWeightsError) {
    Pruner pruner;
    auto result = pruner.prune({}, PruneConfig{});
    EXPECT_FALSE(result.has_value());
}

TEST(Prune, InvalidSparsityError) {
    Pruner pruner;
    PruneConfig cfg;
    cfg.sparsity = 1.5f;
    auto result = pruner.prune({1.0f}, cfg);
    EXPECT_FALSE(result.has_value());
}

// ── Adapt tests ─────────────────────────────────────────────────────────────

TEST(Adapt, LoRAConfigBasic) {
    AdaptConfig cfg;
    cfg.rank = 8;
    cfg.alpha = 16.0f;
    cfg.hidden_dim = 768;
    cfg.target_modules = {"q_proj", "v_proj"};
    cfg.base_model_params = 110000000;  // 110M params

    Adapter adapter;
    auto result = adapter.create_lora_config(cfg);
    ASSERT_TRUE(result.has_value());

    const auto& ar = result.value();

    // 2 modules * 2 * 768 * 8 = 24576 trainable params
    EXPECT_EQ(ar.trainable_params, 2 * 2 * 768 * 8);
    EXPECT_EQ(ar.total_params, 110000000 + ar.trainable_params);
    EXPECT_GT(ar.trainable_ratio, 0.0f);
    EXPECT_LT(ar.trainable_ratio, 0.01f);  // Should be tiny fraction

    // JSON should contain key fields
    EXPECT_NE(ar.config_json.find("\"type\": \"lora\""), std::string::npos);
    EXPECT_NE(ar.config_json.find("\"rank\": 8"), std::string::npos);
    EXPECT_NE(ar.config_json.find("\"q_proj\""), std::string::npos);
}

TEST(Adapt, ZeroRankError) {
    AdaptConfig cfg;
    cfg.rank = 0;
    cfg.target_modules = {"attn"};

    Adapter adapter;
    auto result = adapter.create_lora_config(cfg);
    EXPECT_FALSE(result.has_value());
}

TEST(Adapt, EmptyModulesError) {
    AdaptConfig cfg;
    cfg.rank = 8;
    // no target_modules

    Adapter adapter;
    auto result = adapter.create_lora_config(cfg);
    EXPECT_FALSE(result.has_value());
}

TEST(Adapt, ScalingFactor) {
    AdaptConfig cfg;
    cfg.rank = 4;
    cfg.alpha = 8.0f;
    cfg.hidden_dim = 512;
    cfg.target_modules = {"dense"};

    Adapter adapter;
    auto result = adapter.create_lora_config(cfg);
    ASSERT_TRUE(result.has_value());

    // Scaling = alpha / rank = 8.0 / 4 = 2.0
    EXPECT_NE(result.value().config_json.find("\"scaling\": 2"), std::string::npos);
}
