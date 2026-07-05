// bin/photonics/mzi.cpp
#include "mzi.h"

#include <cmath>

namespace straylight::photonics {

Matrix2x2 MZIUnit::transfer_matrix(const MZI& params) const {
    double ct = std::cos(params.theta / 2.0);
    double st = std::sin(params.theta / 2.0);
    Complex eiphi{std::cos(params.phi), std::sin(params.phi)};

    // Clements decomposition MZI transfer matrix:
    // T = [[e^{i*phi} * cos(theta/2), -sin(theta/2)],
    //      [e^{i*phi} * sin(theta/2),  cos(theta/2)]]
    return {
        eiphi * ct,     Complex{-st, 0},
        eiphi * st,     Complex{ct, 0}
    };
}

Result<std::pair<Complex, Complex>, std::string>
MZIUnit::propagate(Complex in1, Complex in2, const MZI& params) const {
    auto T = transfer_matrix(params);
    Complex out1 = T[0] * in1 + T[1] * in2;
    Complex out2 = T[2] * in1 + T[3] * in2;
    return Result<std::pair<Complex, Complex>, std::string>::ok({out1, out2});
}

} // namespace straylight::photonics
