// bin/photonics/mzi.h
// Mach-Zehnder interferometer unit
#pragma once

#include <array>
#include <complex>
#include <utility>

#include <straylight/result.h>

namespace straylight::photonics {

using Complex = std::complex<double>;
using Matrix2x2 = std::array<Complex, 4>;

/// MZI parameters: internal phase theta, external phase phi.
struct MZI {
    double theta = 0.0;
    double phi = 0.0;
};

class MZIUnit {
public:
    /// Compute the 2x2 transfer matrix for an MZI.
    /// T = [[e^{i*phi} * cos(theta/2), -sin(theta/2)],
    ///      [e^{i*phi} * sin(theta/2),  cos(theta/2)]]
    Matrix2x2 transfer_matrix(const MZI& params) const;

    /// Propagate two input fields through the MZI.
    Result<std::pair<Complex, Complex>, std::string>
    propagate(Complex in1, Complex in2, const MZI& params) const;
};

} // namespace straylight::photonics
