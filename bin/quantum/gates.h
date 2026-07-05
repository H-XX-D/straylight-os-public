// bin/quantum/gates.h
// Standard quantum gate matrices
#pragma once

#include <array>
#include <complex>

namespace straylight::quantum {

using Complex = std::complex<double>;

/// 2x2 matrix stored row-major.
using Matrix2x2 = std::array<Complex, 4>;

/// 4x4 matrix stored row-major.
using Matrix4x4 = std::array<Complex, 16>;

namespace gates {

Matrix2x2 hadamard();
Matrix2x2 pauli_x();
Matrix2x2 pauli_y();
Matrix2x2 pauli_z();
Matrix2x2 phase(double theta);
Matrix2x2 t_gate();
Matrix2x2 rx(double theta);
Matrix2x2 ry(double theta);
Matrix2x2 rz(double theta);
Matrix2x2 identity();

Matrix4x4 cnot();
Matrix4x4 swap();

/// Multiply two 2x2 matrices.
Matrix2x2 mul2(const Matrix2x2& a, const Matrix2x2& b);

/// Kronecker (tensor) product of two 2x2 matrices -> 4x4.
Matrix4x4 kron2(const Matrix2x2& a, const Matrix2x2& b);

} // namespace gates
} // namespace straylight::quantum
