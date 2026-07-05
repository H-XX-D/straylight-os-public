// bin/quantum/circuit.h
// Quantum circuit: sequence of gates applied to a state vector
#pragma once

#include "gates.h"
#include "state_vector.h"

#include <straylight/result.h>

#include <string>
#include <vector>

namespace straylight::quantum {

struct GateOp {
    std::string name;
    std::vector<uint32_t> qubits;
    std::vector<double> params;
};

class Circuit {
public:
    void add_gate(const std::string& name, std::vector<uint32_t> qubits,
                  std::vector<double> params = {});

    /// Apply all gates sequentially to the state vector.
    Result<void, std::string> execute(StateVector& sv) const;

    [[nodiscard]] size_t gate_count() const { return ops_.size(); }

    [[nodiscard]] std::string to_string() const;

private:
    std::vector<GateOp> ops_;

    /// Apply a single-qubit gate to the state vector.
    static void apply_single(StateVector& sv, uint32_t qubit, const Matrix2x2& mat);

    /// Apply a two-qubit gate to the state vector.
    static void apply_two_qubit(StateVector& sv, uint32_t q0, uint32_t q1,
                                const Matrix4x4& mat);

    /// Resolve a gate name + params to a 2x2 matrix (returns empty on failure).
    static Result<Matrix2x2, std::string> resolve_single(const GateOp& op);

    /// Resolve a two-qubit gate name to a 4x4 matrix.
    static Result<Matrix4x4, std::string> resolve_two_qubit(const GateOp& op);
};

} // namespace straylight::quantum
