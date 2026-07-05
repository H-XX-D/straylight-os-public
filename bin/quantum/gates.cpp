// bin/quantum/gates.cpp
#include "gates.h"

#include <cmath>

namespace straylight::quantum {
namespace gates {

static constexpr double INV_SQRT2 = 0.7071067811865475; // 1/sqrt(2)

Matrix2x2 hadamard() {
    return {Complex{INV_SQRT2, 0}, Complex{INV_SQRT2, 0},
            Complex{INV_SQRT2, 0}, Complex{-INV_SQRT2, 0}};
}

Matrix2x2 pauli_x() {
    return {Complex{0, 0}, Complex{1, 0},
            Complex{1, 0}, Complex{0, 0}};
}

Matrix2x2 pauli_y() {
    return {Complex{0, 0}, Complex{0, -1},
            Complex{0, 1}, Complex{0, 0}};
}

Matrix2x2 pauli_z() {
    return {Complex{1, 0}, Complex{0, 0},
            Complex{0, 0}, Complex{-1, 0}};
}

Matrix2x2 phase(double theta) {
    return {Complex{1, 0}, Complex{0, 0},
            Complex{0, 0}, Complex{std::cos(theta), std::sin(theta)}};
}

Matrix2x2 t_gate() {
    return phase(M_PI / 4.0);
}

Matrix2x2 rx(double theta) {
    double c = std::cos(theta / 2.0);
    double s = std::sin(theta / 2.0);
    return {Complex{c, 0}, Complex{0, -s},
            Complex{0, -s}, Complex{c, 0}};
}

Matrix2x2 ry(double theta) {
    double c = std::cos(theta / 2.0);
    double s = std::sin(theta / 2.0);
    return {Complex{c, 0}, Complex{-s, 0},
            Complex{s, 0}, Complex{c, 0}};
}

Matrix2x2 rz(double theta) {
    return {Complex{std::cos(theta / 2.0), -std::sin(theta / 2.0)}, Complex{0, 0},
            Complex{0, 0}, Complex{std::cos(theta / 2.0), std::sin(theta / 2.0)}};
}

Matrix2x2 identity() {
    return {Complex{1, 0}, Complex{0, 0},
            Complex{0, 0}, Complex{1, 0}};
}

Matrix4x4 cnot() {
    // Control on qubit 0, target on qubit 1
    // |00>->|00>, |01>->|01>, |10>->|11>, |11>->|10>
    Matrix4x4 m{};
    m[0]  = {1, 0}; // (0,0)
    m[5]  = {1, 0}; // (1,1)
    m[11] = {1, 0}; // (2,3)
    m[14] = {1, 0}; // (3,2)
    return m;
}

Matrix4x4 swap() {
    // |00>->|00>, |01>->|10>, |10>->|01>, |11>->|11>
    Matrix4x4 m{};
    m[0]  = {1, 0}; // (0,0)
    m[6]  = {1, 0}; // (1,2)
    m[9]  = {1, 0}; // (2,1)
    m[15] = {1, 0}; // (3,3)
    return m;
}

Matrix2x2 mul2(const Matrix2x2& a, const Matrix2x2& b) {
    // C[i][j] = sum_k A[i][k] * B[k][j]
    Matrix2x2 c{};
    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 2; ++j) {
            for (int k = 0; k < 2; ++k) {
                c[i * 2 + j] += a[i * 2 + k] * b[k * 2 + j];
            }
        }
    }
    return c;
}

Matrix4x4 kron2(const Matrix2x2& a, const Matrix2x2& b) {
    // (A kron B)[i*2+k][j*2+l] = A[i][j] * B[k][l]
    Matrix4x4 c{};
    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 2; ++j) {
            for (int k = 0; k < 2; ++k) {
                for (int l = 0; l < 2; ++l) {
                    c[(i * 2 + k) * 4 + (j * 2 + l)] = a[i * 2 + j] * b[k * 2 + l];
                }
            }
        }
    }
    return c;
}

} // namespace gates
} // namespace straylight::quantum
