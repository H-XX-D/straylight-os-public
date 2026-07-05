// bin/quantum/state_vector.cpp
#include "state_vector.h"

#include <cmath>
#include <stdexcept>

namespace straylight::quantum {

StateVector::StateVector(uint32_t num_qubits)
    : num_qubits_(num_qubits),
      amplitudes_(static_cast<size_t>(1) << num_qubits, {0.0, 0.0}) {
    if (num_qubits == 0 || num_qubits > 24) {
        throw std::invalid_argument(
            "num_qubits must be in [1, 24], got " + std::to_string(num_qubits));
    }
    // Initialize to |0...0> state.
    amplitudes_[0] = {1.0, 0.0};
}

std::complex<double>& StateVector::operator[](size_t i) {
    return amplitudes_.at(i);
}

const std::complex<double>& StateVector::operator[](size_t i) const {
    return amplitudes_.at(i);
}

void StateVector::reset() {
    std::fill(amplitudes_.begin(), amplitudes_.end(), std::complex<double>{0.0, 0.0});
    amplitudes_[0] = {1.0, 0.0};
}

std::vector<double> StateVector::probabilities() const {
    std::vector<double> probs(amplitudes_.size());
    for (size_t i = 0; i < amplitudes_.size(); ++i) {
        probs[i] = std::norm(amplitudes_[i]); // |a|^2
    }
    return probs;
}

} // namespace straylight::quantum
