// bin/morph/adapt.h
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <straylight/result.h>

namespace straylight::morph {

struct AdaptConfig {
    uint32_t rank = 8;
    float alpha = 16.0f;
    std::vector<std::string> target_modules;
    /// Assumed hidden dimension of each target module (for parameter counting).
    uint32_t hidden_dim = 768;
    /// Total parameter count of the base model.
    size_t base_model_params = 0;
};

struct AdaptResult {
    std::string config_json;
    size_t trainable_params;
    size_t total_params;
    float trainable_ratio;
};

class Adapter {
public:
    /// Create a LoRA configuration from the given AdaptConfig.
    /// Computes trainable parameter counts and serializes to JSON.
    Result<AdaptResult, std::string> create_lora_config(const AdaptConfig& cfg);

private:
    /// Compute the number of trainable parameters introduced by LoRA.
    /// For each target module: params = 2 * hidden_dim * rank (A and B matrices).
    static size_t compute_trainable_params(uint32_t hidden_dim, uint32_t rank,
                                           size_t num_modules);

    /// Serialize the LoRA config to JSON.
    static std::string serialize_config(const AdaptConfig& cfg,
                                        size_t trainable_params,
                                        size_t total_params,
                                        float trainable_ratio);
};

} // namespace straylight::morph
