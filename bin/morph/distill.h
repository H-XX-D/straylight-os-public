// bin/morph/distill.h
#pragma once

#include <string>
#include <cstdint>

#include <straylight/result.h>

namespace straylight::morph {

struct DistillConfig {
    float temperature = 4.0f;
    float alpha = 0.7f;           // weight for distillation loss vs hard-label loss
    std::string teacher_path;
    std::string student_path;
    uint32_t epochs = 10;
    float learning_rate = 1e-3f;
};

struct DistillResult {
    std::string config_json;  // serialized config for the training loop
};

class Distiller {
public:
    /// Validate config and prepare a distillation configuration.
    /// Returns serialized JSON config for the downstream training loop.
    Result<DistillResult, std::string> prepare(const DistillConfig& cfg);

private:
    /// Compute the soft-label scaling factor: T^2 * alpha
    /// (used to weight the KL-divergence term in the combined loss).
    static float compute_soft_label_scale(float temperature, float alpha);

    /// Serialize config to JSON string.
    static std::string serialize_config(const DistillConfig& cfg,
                                        float soft_label_scale);
};

} // namespace straylight::morph
