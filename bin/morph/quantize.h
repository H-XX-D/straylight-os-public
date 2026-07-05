// bin/morph/quantize.h
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <straylight/result.h>

namespace straylight::morph {

enum class QuantMode { Int8, Int4, Mixed };

struct QuantConfig {
    QuantMode mode = QuantMode::Int8;
    float calibration_percentile = 99.99f;
    bool per_channel = true;
};

struct QuantResult {
    std::vector<int8_t> quantized;
    float scale;
    int32_t zero_point;
    QuantMode mode;
    size_t original_size;
};

class Quantizer {
public:
    /// Quantize a flat weight vector according to cfg.
    Result<QuantResult, std::string> quantize(const std::vector<float>& weights,
                                              QuantConfig cfg);

private:
    /// Compute the clipped min/max using percentile clipping.
    std::pair<float, float> compute_clipped_range(const std::vector<float>& weights,
                                                  float percentile);

    /// Compute scale and zero-point for asymmetric quantization.
    std::pair<float, int32_t> compute_scale_zp(float min_val, float max_val,
                                               int qmin, int qmax);
};

} // namespace straylight::morph
