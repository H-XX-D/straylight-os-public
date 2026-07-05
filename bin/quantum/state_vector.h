// bin/quantum/state_vector.h
// Quantum state vector: 2^n complex amplitudes
#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace straylight::quantum {

class StateVector {
public:
    explicit StateVector(uint32_t num_qubits);

    std::complex<double>& operator[](size_t i);
    const std::complex<double>& operator[](size_t i) const;

    [[nodiscard]] size_t dim() const { return amplitudes_.size(); }
    [[nodiscard]] uint32_t num_qubits() const { return num_qubits_; }

    void reset();

    /// Returns probability of each basis state.
    [[nodiscard]] std::vector<double> probabilities() const;

    /// Raw access for gate application.
    std::vector<std::complex<double>>& data() { return amplitudes_; }
    const std::vector<std::complex<double>>& data() const { return amplitudes_; }

private:
    uint32_t num_qubits_;
    std::vector<std::complex<double>> amplitudes_;
};

} // namespace straylight::quantum
