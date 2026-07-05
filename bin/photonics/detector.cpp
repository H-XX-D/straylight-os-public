// bin/photonics/detector.cpp
#include "detector.h"

#include <algorithm>
#include <cmath>
#include <random>

namespace straylight::photonics {

Result<std::vector<double>, std::string>
Detector::detect(const std::vector<Complex>& amplitudes,
                 double efficiency,
                 double dark_count_rate) const {
    if (amplitudes.empty()) {
        return Result<std::vector<double>, std::string>::error("Empty amplitude vector");
    }
    if (efficiency < 0.0 || efficiency > 1.0) {
        return Result<std::vector<double>, std::string>::error(
            "Efficiency must be in [0, 1], got " + std::to_string(efficiency));
    }
    if (dark_count_rate < 0.0 || dark_count_rate > 1.0) {
        return Result<std::vector<double>, std::string>::error(
            "Dark count rate must be in [0, 1], got " + std::to_string(dark_count_rate));
    }

    std::vector<double> detections(amplitudes.size());
    for (size_t i = 0; i < amplitudes.size(); ++i) {
        double photon_prob = std::norm(amplitudes[i]); // |a|^2
        // Detection probability = efficiency * photon_prob + dark_count * (1 - efficiency * photon_prob)
        // Simplified: p_detect = eta*p + d*(1 - eta*p)
        double p_signal = efficiency * photon_prob;
        double p_detect = p_signal + dark_count_rate * (1.0 - p_signal);
        detections[i] = std::clamp(p_detect, 0.0, 1.0);
    }

    return Result<std::vector<double>, std::string>::ok(std::move(detections));
}

Result<std::vector<uint32_t>, std::string>
Detector::sample_clicks(const std::vector<Complex>& amplitudes,
                        double efficiency,
                        double dark_count_rate) const {
    auto probs_result = detect(amplitudes, efficiency, dark_count_rate);
    if (!probs_result.has_value()) {
        return Result<std::vector<uint32_t>, std::string>::error(probs_result.error());
    }

    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_real_distribution<double> dist(0.0, 1.0);

    const auto& probs = probs_result.value();
    std::vector<uint32_t> clicks(probs.size());
    for (size_t i = 0; i < probs.size(); ++i) {
        clicks[i] = (dist(rng) < probs[i]) ? 1 : 0;
    }

    return Result<std::vector<uint32_t>, std::string>::ok(std::move(clicks));
}

} // namespace straylight::photonics
