// bin/quantum/circuit.cpp
#include "circuit.h"

#include <cmath>
#include <sstream>

namespace straylight::quantum {

void Circuit::add_gate(const std::string& name, std::vector<uint32_t> qubits,
                       std::vector<double> params) {
    ops_.push_back({name, std::move(qubits), std::move(params)});
}

Result<void, std::string> Circuit::execute(StateVector& sv) const {
    for (size_t i = 0; i < ops_.size(); ++i) {
        const auto& op = ops_[i];
        if (op.qubits.empty()) {
            return Result<void, std::string>::error(
                "Gate " + std::to_string(i) + " (" + op.name + "): no qubits specified");
        }

        // Validate qubit indices.
        for (auto q : op.qubits) {
            if (q >= sv.num_qubits()) {
                return Result<void, std::string>::error(
                    "Gate " + std::to_string(i) + " (" + op.name +
                    "): qubit " + std::to_string(q) +
                    " out of range [0," + std::to_string(sv.num_qubits()) + ")");
            }
        }

        if (op.qubits.size() == 1) {
            auto mat = resolve_single(op);
            if (!mat.has_value()) {
                return Result<void, std::string>::error(mat.error());
            }
            apply_single(sv, op.qubits[0], mat.value());
        } else if (op.qubits.size() == 2) {
            auto mat = resolve_two_qubit(op);
            if (!mat.has_value()) {
                return Result<void, std::string>::error(mat.error());
            }
            apply_two_qubit(sv, op.qubits[0], op.qubits[1], mat.value());
        } else {
            return Result<void, std::string>::error(
                "Gate " + op.name + ": 3+ qubit gates not supported");
        }
    }
    return Result<void, std::string>::ok();
}

std::string Circuit::to_string() const {
    std::ostringstream oss;
    for (size_t i = 0; i < ops_.size(); ++i) {
        const auto& op = ops_[i];
        oss << i << ": " << op.name << "(";
        for (size_t j = 0; j < op.qubits.size(); ++j) {
            if (j > 0) oss << ",";
            oss << "q" << op.qubits[j];
        }
        if (!op.params.empty()) {
            oss << " | ";
            for (size_t j = 0; j < op.params.size(); ++j) {
                if (j > 0) oss << ",";
                oss << op.params[j];
            }
        }
        oss << ")\n";
    }
    return oss.str();
}

void Circuit::apply_single(StateVector& sv, uint32_t qubit, const Matrix2x2& mat) {
    size_t n = sv.dim();
    uint32_t nq = sv.num_qubits();
    // The qubit index in our convention: qubit 0 is the most significant bit.
    // We iterate over all basis states. For each pair differing only in the target qubit:
    size_t step = static_cast<size_t>(1) << (nq - 1 - qubit);
    for (size_t block = 0; block < n; block += step * 2) {
        for (size_t i = 0; i < step; ++i) {
            size_t idx0 = block + i;          // qubit = 0
            size_t idx1 = block + i + step;   // qubit = 1
            auto a0 = sv[idx0];
            auto a1 = sv[idx1];
            sv[idx0] = mat[0] * a0 + mat[1] * a1;
            sv[idx1] = mat[2] * a0 + mat[3] * a1;
        }
    }
}

void Circuit::apply_two_qubit(StateVector& sv, uint32_t q0, uint32_t q1,
                               const Matrix4x4& mat) {
    size_t n = sv.dim();
    uint32_t nq = sv.num_qubits();
    size_t bit0 = static_cast<size_t>(1) << (nq - 1 - q0);
    size_t bit1 = static_cast<size_t>(1) << (nq - 1 - q1);

    // For every basis state, compute which "slot" (00,01,10,11) the two qubits occupy,
    // and apply the 4x4 matrix to groups of 4.
    // We iterate over basis states with both target qubits = 0, then derive the other 3.
    for (size_t base = 0; base < n; ++base) {
        // Skip if either target bit is set (we handle them from the 00 variant).
        if ((base & bit0) || (base & bit1)) continue;

        size_t i00 = base;
        size_t i01 = base | bit1;
        size_t i10 = base | bit0;
        size_t i11 = base | bit0 | bit1;

        Complex a[4] = {sv[i00], sv[i01], sv[i10], sv[i11]};
        Complex r[4] = {};
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 4; ++col) {
                r[row] += mat[row * 4 + col] * a[col];
            }
        }
        sv[i00] = r[0];
        sv[i01] = r[1];
        sv[i10] = r[2];
        sv[i11] = r[3];
    }
}

Result<Matrix2x2, std::string> Circuit::resolve_single(const GateOp& op) {
    const auto& name = op.name;
    if (name == "h" || name == "H" || name == "hadamard") return Result<Matrix2x2, std::string>::ok(gates::hadamard());
    if (name == "x" || name == "X" || name == "pauli_x")  return Result<Matrix2x2, std::string>::ok(gates::pauli_x());
    if (name == "y" || name == "Y" || name == "pauli_y")  return Result<Matrix2x2, std::string>::ok(gates::pauli_y());
    if (name == "z" || name == "Z" || name == "pauli_z")  return Result<Matrix2x2, std::string>::ok(gates::pauli_z());
    if (name == "t" || name == "T" || name == "t_gate")    return Result<Matrix2x2, std::string>::ok(gates::t_gate());
    if (name == "s" || name == "S") return Result<Matrix2x2, std::string>::ok(gates::phase(M_PI / 2.0));
    if (name == "phase" || name == "p" || name == "P") {
        if (op.params.empty()) {
            return Result<Matrix2x2, std::string>::error("phase gate requires theta parameter");
        }
        return Result<Matrix2x2, std::string>::ok(gates::phase(op.params[0]));
    }
    if (name == "rx" || name == "Rx" || name == "RX") {
        if (op.params.empty()) {
            return Result<Matrix2x2, std::string>::error("rx gate requires theta parameter");
        }
        return Result<Matrix2x2, std::string>::ok(gates::rx(op.params[0]));
    }
    if (name == "ry" || name == "Ry" || name == "RY") {
        if (op.params.empty()) {
            return Result<Matrix2x2, std::string>::error("ry gate requires theta parameter");
        }
        return Result<Matrix2x2, std::string>::ok(gates::ry(op.params[0]));
    }
    if (name == "rz" || name == "Rz" || name == "RZ") {
        if (op.params.empty()) {
            return Result<Matrix2x2, std::string>::error("rz gate requires theta parameter");
        }
        return Result<Matrix2x2, std::string>::ok(gates::rz(op.params[0]));
    }
    if (name == "id" || name == "I" || name == "identity") return Result<Matrix2x2, std::string>::ok(gates::identity());

    return Result<Matrix2x2, std::string>::error("Unknown single-qubit gate: " + name);
}

Result<Matrix4x4, std::string> Circuit::resolve_two_qubit(const GateOp& op) {
    const auto& name = op.name;
    if (name == "cx" || name == "CX" || name == "cnot" || name == "CNOT")
        return Result<Matrix4x4, std::string>::ok(gates::cnot());
    if (name == "swap" || name == "SWAP")
        return Result<Matrix4x4, std::string>::ok(gates::swap());
    return Result<Matrix4x4, std::string>::error("Unknown two-qubit gate: " + name);
}

} // namespace straylight::quantum
