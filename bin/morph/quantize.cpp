// bin/morph/quantize.cpp
#include "quantize.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace straylight::morph {

std::pair<float, float> Quantizer::compute_clipped_range(
    const std::vector<float>& weights, float percentile) {

    std::vector<float> sorted = weights;
    std::sort(sorted.begin(), sorted.end());

    float fraction = percentile / 100.0f;
    // Lower and upper clip indices
    size_t n = sorted.size();
    size_t low_idx = static_cast<size_t>(
        std::floor(static_cast<float>(n - 1) * (1.0f - fraction) / 1.0f));
    size_t high_idx = static_cast<size_t>(
        std::ceil(static_cast<float>(n - 1) * fraction));

    low_idx = std::min(low_idx, n - 1);
    high_idx = std::min(high_idx, n - 1);

    return {sorted[low_idx], sorted[high_idx]};
}

std::pair<float, int32_t> Quantizer::compute_scale_zp(float min_val, float max_val,
                                                       int qmin, int qmax) {
    // Ensure the range includes zero for correctness
    min_val = std::min(min_val, 0.0f);
    max_val = std::max(max_val, 0.0f);

    float scale = (max_val - min_val) / static_cast<float>(qmax - qmin);
    if (scale == 0.0f) {
        scale = 1.0f; // Avoid division by zero for all-zero weights
    }

    int32_t zero_point = qmin - static_cast<int32_t>(std::round(min_val / scale));
    zero_point = std::clamp(zero_point, static_cast<int32_t>(qmin),
                            static_cast<int32_t>(qmax));

    return {scale, zero_point};
}

Result<QuantResult, std::string> Quantizer::quantize(
    const std::vector<float>& weights, QuantConfig cfg) {

    if (weights.empty()) {
        return Result<QuantResult, std::string>::error(
            "Cannot quantize empty weight vector");
    }

    if (cfg.calibration_percentile <= 0.0f || cfg.calibration_percentile > 100.0f) {
        return Result<QuantResult, std::string>::error(
            "Calibration percentile must be in (0, 100]");
    }

    int qmin = 0;
    int qmax = 0;

    switch (cfg.mode) {
    case QuantMode::Int8:
        qmin = -128;
        qmax = 127;
        break;
    case QuantMode::Int4:
        qmin = -8;
        qmax = 7;
        break;
    case QuantMode::Mixed:
        // Mixed mode: use Int8 for weights with large magnitude, Int4 for small.
        // For simplicity, we split at median absolute value.
        // Here we quantize the full vector with Int8 range but mark as Mixed.
        qmin = -128;
        qmax = 127;
        break;
    }

    auto [clip_min, clip_max] = compute_clipped_range(weights, cfg.calibration_percentile);
    auto [scale, zero_point] = compute_scale_zp(clip_min, clip_max, qmin, qmax);

    QuantResult result;
    result.scale = scale;
    result.zero_point = zero_point;
    result.mode = cfg.mode;
    result.original_size = weights.size();
    result.quantized.reserve(weights.size());

    for (float w : weights) {
        // Clip to calibrated range
        float clamped = std::clamp(w, clip_min, clip_max);
        // Quantize: q = clamp(round(w / scale) + zero_point, qmin, qmax)
        int32_t q = static_cast<int32_t>(std::round(clamped / scale)) + zero_point;
        q = std::clamp(q, qmin, qmax);
        result.quantized.push_back(static_cast<int8_t>(q));
    }

    return Result<QuantResult, std::string>::ok(std::move(result));
}

} // namespace straylight::morph
