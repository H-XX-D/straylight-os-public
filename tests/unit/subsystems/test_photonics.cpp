// tests/unit/subsystems/test_photonics.cpp
// Photonics subsystem tests: MZI transfer matrix, mesh forward propagation.

#include <gtest/gtest.h>

#include "mzi.h"
#include "mesh.h"
#include "detector.h"

#include <cmath>
#include <complex>
#include <numeric>

using namespace straylight::photonics;

// ---------------------------------------------------------------------------
// MZI tests
// ---------------------------------------------------------------------------

TEST(MZI, IdentityAtZeroAngles) {
    MZIUnit unit;
    MZI params{0.0, 0.0};
    auto T = unit.transfer_matrix(params);

    // At theta=0, phi=0: T = [[1, 0], [0, 1]] (identity-like).
    // cos(0) = 1, sin(0) = 0, e^{i*0} = 1.
    EXPECT_NEAR(T[0].real(), 1.0, 1e-12); // (0,0)
    EXPECT_NEAR(std::abs(T[1]), 0.0, 1e-12); // (0,1)
    EXPECT_NEAR(std::abs(T[2]), 0.0, 1e-12); // (1,0)
    EXPECT_NEAR(T[3].real(), 1.0, 1e-12); // (1,1)
}

TEST(MZI, BalancedSplitterAtPiOver2) {
    MZIUnit unit;
    MZI params{M_PI, 0.0}; // theta = pi -> 50/50 splitter
    auto T = unit.transfer_matrix(params);

    // cos(pi/2) = 0, sin(pi/2) = 1
    // T = [[0, -1], [1, 0]] (swap with phase)
    EXPECT_NEAR(std::abs(T[0]), 0.0, 1e-10);  // cos(pi/2) ~ 0
    EXPECT_NEAR(T[1].real(), -1.0, 1e-10);     // -sin(pi/2) = -1
    EXPECT_NEAR(T[2].real(), 1.0, 1e-10);      // sin(pi/2) = 1
    EXPECT_NEAR(std::abs(T[3]), 0.0, 1e-10);   // cos(pi/2) ~ 0
}

TEST(MZI, PropagateConservesEnergy) {
    MZIUnit unit;
    MZI params{M_PI / 4.0, 0.3};

    Complex in1{1.0, 0.0};
    Complex in2{0.0, 0.0};

    auto result = unit.propagate(in1, in2, params);
    ASSERT_TRUE(result.has_value());

    double input_power = std::norm(in1) + std::norm(in2);
    double output_power = std::norm(result.value().first) + std::norm(result.value().second);
    EXPECT_NEAR(output_power, input_power, 1e-10);
}

TEST(MZI, TransferMatrixIsUnitary) {
    MZIUnit unit;
    MZI params{1.23, 0.45};
    auto T = unit.transfer_matrix(params);

    // For a unitary 2x2 matrix: T * T^dag = I.
    // T^dag = [[T[0]*, T[2]*], [T[1]*, T[3]*]]
    Complex r00 = T[0] * std::conj(T[0]) + T[1] * std::conj(T[1]);
    Complex r11 = T[2] * std::conj(T[2]) + T[3] * std::conj(T[3]);
    Complex r01 = T[0] * std::conj(T[2]) + T[1] * std::conj(T[3]);

    EXPECT_NEAR(r00.real(), 1.0, 1e-10);
    EXPECT_NEAR(r11.real(), 1.0, 1e-10);
    EXPECT_NEAR(std::abs(r01), 0.0, 1e-10);
}

// ---------------------------------------------------------------------------
// Mesh tests
// ---------------------------------------------------------------------------

TEST(Mesh, ForwardConservesEnergy) {
    PhotonicMesh mesh;
    mesh.set_size(4, 3);

    // Set some non-trivial MZI parameters.
    mesh.set_mzi(0, 0, {M_PI / 3.0, 0.1});
    mesh.set_mzi(2, 0, {M_PI / 4.0, 0.2});
    mesh.set_mzi(1, 1, {M_PI / 6.0, 0.3});
    mesh.set_mzi(0, 2, {M_PI / 5.0, 0.4});
    mesh.set_mzi(2, 2, {M_PI / 7.0, 0.5});

    std::vector<Complex> input = {{1.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}};
    auto result = mesh.forward(input);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result.value().size(), 4u);

    double input_power = 0.0;
    for (auto& c : input) input_power += std::norm(c);

    double output_power = 0.0;
    for (auto& c : result.value()) output_power += std::norm(c);

    EXPECT_NEAR(output_power, input_power, 1e-10);
}

TEST(Mesh, InputSizeMismatchReturnsError) {
    PhotonicMesh mesh;
    mesh.set_size(4, 3);

    std::vector<Complex> wrong_input = {{1.0, 0.0}, {0.0, 0.0}}; // Only 2, need 4.
    auto result = mesh.forward(wrong_input);
    EXPECT_FALSE(result.has_value());
}

TEST(Mesh, IdentityMeshPassesThrough) {
    PhotonicMesh mesh;
    mesh.set_size(4, 2);
    // All MZIs at theta=0: identity transformation.

    std::vector<Complex> input = {{0.5, 0.3}, {0.2, -0.1}, {0.7, 0.0}, {0.0, 0.4}};
    auto result = mesh.forward(input);
    ASSERT_TRUE(result.has_value());

    for (size_t i = 0; i < input.size(); ++i) {
        EXPECT_NEAR(result.value()[i].real(), input[i].real(), 1e-10);
        EXPECT_NEAR(result.value()[i].imag(), input[i].imag(), 1e-10);
    }
}

// ---------------------------------------------------------------------------
// Detector tests
// ---------------------------------------------------------------------------

TEST(Detector, PerfectDetectorReturnsSquaredAmplitudes) {
    Detector det;
    std::vector<Complex> amps = {{1.0, 0.0}, {0.0, 0.0}};
    auto result = det.detect(amps, 1.0, 0.0); // Perfect efficiency, no dark counts.
    ASSERT_TRUE(result.has_value());
    EXPECT_NEAR(result.value()[0], 1.0, 1e-10);
    EXPECT_NEAR(result.value()[1], 0.0, 1e-10);
}

TEST(Detector, DarkCountsAddBaseline) {
    Detector det;
    std::vector<Complex> amps = {{0.0, 0.0}, {0.0, 0.0}};
    auto result = det.detect(amps, 0.95, 0.01);
    ASSERT_TRUE(result.has_value());
    // With no signal, detection probability = dark count rate.
    EXPECT_NEAR(result.value()[0], 0.01, 1e-10);
    EXPECT_NEAR(result.value()[1], 0.01, 1e-10);
}

TEST(Detector, InvalidEfficiencyReturnsError) {
    Detector det;
    std::vector<Complex> amps = {{1.0, 0.0}};
    auto result = det.detect(amps, 1.5, 0.0); // Invalid efficiency.
    EXPECT_FALSE(result.has_value());
}
