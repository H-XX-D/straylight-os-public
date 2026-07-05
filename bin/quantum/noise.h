// bin/quantum/noise.h
// Quantum noise models using Kraus operators
#pragma once

#include "gates.h"
#include "state_vector.h"

#include <straylight/result.h>

#include <string>
#include <vector>

namespace straylight::quantum {

/// Noise channel described by Kraus operators {K_i} where sum(K_i^dag K_i) = I.
struct NoiseChannel {
    std::string name;
    std::vector<Matrix2x2> kraus_ops;
};

class NoiseModel {
public:
    /// Depolarizing channel: with probability p, apply random Pauli.
    void add_depolarizing(double p);

    /// Dephasing channel: with probability p, apply Z.
    void add_dephasing(double p);

    /// Amplitude damping: decay from |1> to |0> with rate gamma.
    void add_amplitude_damping(double gamma);

    /// Apply all noise channels to a specific qubit.
    /// Uses density-matrix-via-state-vector simulation:
    /// randomly samples a Kraus operator weighted by probability.
    Result<void, std::string> apply(StateVector& sv, uint32_t qubit);

    [[nodiscard]] size_t channel_count() const { return channels_.size(); }

private:
    std::vector<NoiseChannel> channels_;

    /// Apply a single-qubit matrix to the state vector (same as Circuit::apply_single).
    static void apply_kraus(StateVector& sv, uint32_t qubit, const Matrix2x2& mat);
};

} // namespace straylight::quantum
