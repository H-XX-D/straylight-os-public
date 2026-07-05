// bin/quantum/measure.h
// Quantum measurement: collapse, sampling, expectation values
#pragma once

#include "gates.h"
#include "state_vector.h"

#include <straylight/result.h>

#include <string>
#include <vector>

namespace straylight::quantum {

class Measurement {
public:
    /// Measure a single qubit, collapsing the state. Returns 0 or 1.
    static Result<uint32_t, std::string> measure_qubit(StateVector& sv, uint32_t qubit);

    /// Sample the full state vector `shots` times without collapsing.
    /// Returns a histogram: each element is a basis state index, appearing
    /// proportionally to its probability.
    static Result<std::vector<uint32_t>, std::string> measure_all(StateVector& sv,
                                                                    uint32_t shots);

    /// Compute the expectation value <psi|O|psi> for a diagonal observable
    /// given as a vector of eigenvalues (one per basis state).
    static double expectation(const StateVector& sv,
                              const std::vector<double>& observable);
};

} // namespace straylight::quantum
