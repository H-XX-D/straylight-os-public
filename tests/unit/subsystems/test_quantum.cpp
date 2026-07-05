// tests/unit/subsystems/test_quantum.cpp
// Quantum subsystem tests: state vector, Hadamard, measurement.

#include <gtest/gtest.h>

#include "state_vector.h"
#include "gates.h"
#include "circuit.h"
#include "measure.h"
#include "noise.h"

#include <cmath>
#include <complex>
#include <numeric>

using namespace straylight::quantum;

// ---------------------------------------------------------------------------
// StateVector tests
// ---------------------------------------------------------------------------

TEST(StateVector, InitializesToZeroState) {
    StateVector sv(3);
    EXPECT_EQ(sv.dim(), 8u);
    EXPECT_EQ(sv.num_qubits(), 3u);

    // |000> should have amplitude 1.
    EXPECT_DOUBLE_EQ(sv[0].real(), 1.0);
    EXPECT_DOUBLE_EQ(sv[0].imag(), 0.0);

    // All other amplitudes should be 0.
    for (size_t i = 1; i < sv.dim(); ++i) {
        EXPECT_DOUBLE_EQ(std::abs(sv[i]), 0.0);
    }
}

TEST(StateVector, Reset) {
    StateVector sv(2);
    sv[0] = {0.0, 0.0};
    sv[1] = {1.0, 0.0};
    sv.reset();
    EXPECT_DOUBLE_EQ(sv[0].real(), 1.0);
    EXPECT_DOUBLE_EQ(std::abs(sv[1]), 0.0);
}

TEST(StateVector, ProbabilitiesSumToOne) {
    StateVector sv(3);
    auto probs = sv.probabilities();
    double sum = std::accumulate(probs.begin(), probs.end(), 0.0);
    EXPECT_NEAR(sum, 1.0, 1e-12);
}

// ---------------------------------------------------------------------------
// Gate matrix tests
// ---------------------------------------------------------------------------

TEST(Gates, HadamardIsUnitary) {
    auto H = gates::hadamard();
    auto HH = gates::mul2(H, H);
    // H*H should be identity.
    EXPECT_NEAR(HH[0].real(), 1.0, 1e-12);
    EXPECT_NEAR(HH[3].real(), 1.0, 1e-12);
    EXPECT_NEAR(std::abs(HH[1]), 0.0, 1e-12);
    EXPECT_NEAR(std::abs(HH[2]), 0.0, 1e-12);
}

TEST(Gates, PauliXSwaps01) {
    auto X = gates::pauli_x();
    // X|0> = |1>: X * [1,0] = [0,1]
    EXPECT_NEAR(std::abs(X[0]), 0.0, 1e-12);
    EXPECT_NEAR(X[1].real(), 1.0, 1e-12);
}

// ---------------------------------------------------------------------------
// Circuit tests
// ---------------------------------------------------------------------------

TEST(Circuit, HadamardOnQubit0) {
    StateVector sv(1);
    Circuit circ;
    circ.add_gate("h", {0});
    auto result = circ.execute(sv);
    ASSERT_TRUE(result.has_value());

    // After H|0>, probabilities should be 50/50.
    auto probs = sv.probabilities();
    EXPECT_NEAR(probs[0], 0.5, 1e-12);
    EXPECT_NEAR(probs[1], 0.5, 1e-12);
}

TEST(Circuit, PauliXFlipsState) {
    StateVector sv(1);
    Circuit circ;
    circ.add_gate("x", {0});
    auto result = circ.execute(sv);
    ASSERT_TRUE(result.has_value());

    // |0> -> |1>
    auto probs = sv.probabilities();
    EXPECT_NEAR(probs[0], 0.0, 1e-12);
    EXPECT_NEAR(probs[1], 1.0, 1e-12);
}

TEST(Circuit, CNOTEntanglement) {
    // H on qubit 0, then CNOT(0,1) -> Bell state.
    StateVector sv(2);
    Circuit circ;
    circ.add_gate("h", {0});
    circ.add_gate("cx", {0, 1});
    auto result = circ.execute(sv);
    ASSERT_TRUE(result.has_value());

    // |00> and |11> each have probability 0.5.
    auto probs = sv.probabilities();
    EXPECT_NEAR(probs[0], 0.5, 1e-10); // |00>
    EXPECT_NEAR(probs[1], 0.0, 1e-10); // |01>
    EXPECT_NEAR(probs[2], 0.0, 1e-10); // |10>
    EXPECT_NEAR(probs[3], 0.5, 1e-10); // |11>
}

TEST(Circuit, InvalidQubitReturnsError) {
    StateVector sv(2);
    Circuit circ;
    circ.add_gate("h", {5}); // qubit 5 doesn't exist
    auto result = circ.execute(sv);
    EXPECT_FALSE(result.has_value());
}

TEST(Circuit, GateCountAndToString) {
    Circuit circ;
    circ.add_gate("h", {0});
    circ.add_gate("cx", {0, 1});
    EXPECT_EQ(circ.gate_count(), 2u);
    std::string s = circ.to_string();
    EXPECT_FALSE(s.empty());
}

// ---------------------------------------------------------------------------
// Measurement tests
// ---------------------------------------------------------------------------

TEST(Measurement, MeasureAllSampling) {
    StateVector sv(2);
    // Apply H to both qubits -> uniform distribution.
    Circuit circ;
    circ.add_gate("h", {0});
    circ.add_gate("h", {1});
    ASSERT_TRUE(circ.execute(sv).has_value());

    auto result = Measurement::measure_all(sv, 10000);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().size(), 10000u);

    // Each basis state should appear ~2500 times.
    std::vector<uint32_t> hist(4, 0);
    for (auto r : result.value()) {
        ASSERT_LT(r, 4u);
        hist[r]++;
    }
    for (auto count : hist) {
        EXPECT_GT(count, 1500u); // Very loose bound.
        EXPECT_LT(count, 3500u);
    }
}

TEST(Measurement, MeasureQubitCollapses) {
    StateVector sv(1);
    Circuit circ;
    circ.add_gate("h", {0}); // 50/50
    ASSERT_TRUE(circ.execute(sv).has_value());

    auto result = Measurement::measure_qubit(sv, 0);
    ASSERT_TRUE(result.has_value());
    uint32_t outcome = result.value();
    ASSERT_TRUE(outcome == 0 || outcome == 1);

    // After measurement, probability should be deterministic.
    auto probs = sv.probabilities();
    EXPECT_NEAR(probs[outcome], 1.0, 1e-10);
    EXPECT_NEAR(probs[1 - outcome], 0.0, 1e-10);
}

TEST(Measurement, Expectation) {
    StateVector sv(1);
    // |0> state: Z expectation = +1.
    std::vector<double> Z_obs = {1.0, -1.0};
    double exp_val = Measurement::expectation(sv, Z_obs);
    EXPECT_NEAR(exp_val, 1.0, 1e-12);

    // Apply X to get |1>: Z expectation = -1.
    Circuit circ;
    circ.add_gate("x", {0});
    ASSERT_TRUE(circ.execute(sv).has_value());
    exp_val = Measurement::expectation(sv, Z_obs);
    EXPECT_NEAR(exp_val, -1.0, 1e-12);
}

// ---------------------------------------------------------------------------
// Noise tests
// ---------------------------------------------------------------------------

TEST(Noise, DepolarizingPreservesNormalization) {
    StateVector sv(1);
    Circuit circ;
    circ.add_gate("h", {0});
    ASSERT_TRUE(circ.execute(sv).has_value());

    NoiseModel noise;
    noise.add_depolarizing(0.1);
    auto result = noise.apply(sv, 0);
    ASSERT_TRUE(result.has_value());

    // Probabilities should still sum to 1.
    auto probs = sv.probabilities();
    double sum = std::accumulate(probs.begin(), probs.end(), 0.0);
    EXPECT_NEAR(sum, 1.0, 1e-10);
}
