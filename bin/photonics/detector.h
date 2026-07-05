// bin/photonics/detector.h
// Photon detector with quantum efficiency and dark counts
#pragma once

#include <complex>
#include <cstdint>
#include <string>
#include <vector>

#include <straylight/result.h>

namespace straylight::photonics {

using Complex = std::complex<double>;

class Detector {
public:
    /// Detect photons from output amplitudes.
    /// Returns detection probabilities per waveguide, accounting for:
    /// - quantum efficiency: probability of detecting a present photon
    /// - dark_count_rate: false detection probability per waveguide
    Result<std::vector<double>, std::string>
    detect(const std::vector<Complex>& amplitudes,
           double efficiency = 0.95,
           double dark_count_rate = 1e-6) const;

    /// Simulate actual click events (0 or 1 per waveguide) based on detection.
    Result<std::vector<uint32_t>, std::string>
    sample_clicks(const std::vector<Complex>& amplitudes,
                  double efficiency = 0.95,
                  double dark_count_rate = 1e-6) const;
};

} // namespace straylight::photonics
