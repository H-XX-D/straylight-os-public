// bin/morph/distill.cpp
#include "distill.h"

#include <cmath>
#include <filesystem>
#include <sstream>

namespace straylight::morph {

float Distiller::compute_soft_label_scale(float temperature, float alpha) {
    // The gradient of KL-divergence with softmax at temperature T is scaled
    // by T^2. We multiply by alpha to weight the distillation loss term.
    return temperature * temperature * alpha;
}

std::string Distiller::serialize_config(const DistillConfig& cfg,
                                        float soft_label_scale) {
    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"teacher_path\": \"" << cfg.teacher_path << "\",\n";
    oss << "  \"student_path\": \"" << cfg.student_path << "\",\n";
    oss << "  \"temperature\": " << cfg.temperature << ",\n";
    oss << "  \"alpha\": " << cfg.alpha << ",\n";
    oss << "  \"hard_label_weight\": " << (1.0f - cfg.alpha) << ",\n";
    oss << "  \"soft_label_scale\": " << soft_label_scale << ",\n";
    oss << "  \"epochs\": " << cfg.epochs << ",\n";
    oss << "  \"learning_rate\": " << cfg.learning_rate << ",\n";
    oss << "  \"loss\": \"combined_kd\",\n";
    oss << "  \"soft_loss\": \"kl_divergence\",\n";
    oss << "  \"hard_loss\": \"cross_entropy\"\n";
    oss << "}";
    return oss.str();
}

Result<DistillResult, std::string> Distiller::prepare(const DistillConfig& cfg) {
    // Validate temperature
    if (cfg.temperature <= 0.0f) {
        return Result<DistillResult, std::string>::error(
            "Temperature must be positive, got: " + std::to_string(cfg.temperature));
    }

    // Validate alpha
    if (cfg.alpha < 0.0f || cfg.alpha > 1.0f) {
        return Result<DistillResult, std::string>::error(
            "Alpha must be in [0, 1], got: " + std::to_string(cfg.alpha));
    }

    // Validate teacher path exists
    if (cfg.teacher_path.empty()) {
        return Result<DistillResult, std::string>::error(
            "Teacher model path is empty");
    }
    if (!std::filesystem::exists(cfg.teacher_path)) {
        return Result<DistillResult, std::string>::error(
            "Teacher model not found: " + cfg.teacher_path);
    }

    // Validate student path exists
    if (cfg.student_path.empty()) {
        return Result<DistillResult, std::string>::error(
            "Student model path is empty");
    }
    if (!std::filesystem::exists(cfg.student_path)) {
        return Result<DistillResult, std::string>::error(
            "Student model not found: " + cfg.student_path);
    }

    // Compute scaling parameters
    float soft_label_scale = compute_soft_label_scale(cfg.temperature, cfg.alpha);

    // Serialize configuration
    DistillResult result;
    result.config_json = serialize_config(cfg, soft_label_scale);

    return Result<DistillResult, std::string>::ok(std::move(result));
}

} // namespace straylight::morph
