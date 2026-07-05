// bin/morph/adapt.cpp
#include "adapt.h"

#include <sstream>

namespace straylight::morph {

size_t Adapter::compute_trainable_params(uint32_t hidden_dim, uint32_t rank,
                                         size_t num_modules) {
    // Each LoRA module adds two low-rank matrices:
    //   A: [hidden_dim x rank]  (down-projection)
    //   B: [rank x hidden_dim]  (up-projection)
    // Total per module = 2 * hidden_dim * rank
    return 2 * static_cast<size_t>(hidden_dim) * static_cast<size_t>(rank) * num_modules;
}

std::string Adapter::serialize_config(const AdaptConfig& cfg,
                                      size_t trainable_params,
                                      size_t total_params,
                                      float trainable_ratio) {
    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"type\": \"lora\",\n";
    oss << "  \"rank\": " << cfg.rank << ",\n";
    oss << "  \"alpha\": " << cfg.alpha << ",\n";
    oss << "  \"scaling\": " << (cfg.alpha / static_cast<float>(cfg.rank)) << ",\n";
    oss << "  \"hidden_dim\": " << cfg.hidden_dim << ",\n";
    oss << "  \"target_modules\": [";
    for (size_t i = 0; i < cfg.target_modules.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << "\"" << cfg.target_modules[i] << "\"";
    }
    oss << "],\n";
    oss << "  \"trainable_params\": " << trainable_params << ",\n";
    oss << "  \"total_params\": " << total_params << ",\n";
    oss << "  \"trainable_ratio\": " << trainable_ratio << ",\n";
    oss << "  \"dropout\": 0.0,\n";
    oss << "  \"bias\": \"none\"\n";
    oss << "}";
    return oss.str();
}

Result<AdaptResult, std::string> Adapter::create_lora_config(const AdaptConfig& cfg) {
    if (cfg.rank == 0) {
        return Result<AdaptResult, std::string>::error(
            "LoRA rank must be > 0");
    }

    if (cfg.alpha <= 0.0f) {
        return Result<AdaptResult, std::string>::error(
            "LoRA alpha must be > 0");
    }

    if (cfg.target_modules.empty()) {
        return Result<AdaptResult, std::string>::error(
            "At least one target module must be specified");
    }

    if (cfg.hidden_dim == 0) {
        return Result<AdaptResult, std::string>::error(
            "Hidden dimension must be > 0");
    }

    size_t trainable = compute_trainable_params(cfg.hidden_dim, cfg.rank,
                                                 cfg.target_modules.size());

    // Total params = base model params + LoRA trainable params
    // If base_model_params is 0, we report just the LoRA params as total
    size_t total = (cfg.base_model_params > 0)
                       ? cfg.base_model_params + trainable
                       : trainable;

    float ratio = (total > 0)
                      ? static_cast<float>(trainable) / static_cast<float>(total)
                      : 0.0f;

    AdaptResult result;
    result.trainable_params = trainable;
    result.total_params = total;
    result.trainable_ratio = ratio;
    result.config_json = serialize_config(cfg, trainable, total, ratio);

    return Result<AdaptResult, std::string>::ok(std::move(result));
}

} // namespace straylight::morph
