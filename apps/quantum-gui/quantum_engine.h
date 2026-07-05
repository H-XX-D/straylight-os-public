// apps/quantum-gui/quantum_engine.h
// Real statevector quantum simulator engine (no imgui, no GUI deps).
// Pure C++20 math: holds amp = std::vector<std::complex<double>> of size 2^n,
// applies single- and two-qubit gates as exact unitaries, and produces the
// derived quantities the panel renders. Shared by quantum_panel.cpp and the
// standalone correctness harness so the math under test is the math shipped.
#pragma once
#include <vector>
#include <string>
#include <complex>
#include <cmath>
#include <cstdint>
#include <random>
#include <algorithm>
#include <utility>

namespace straylight::quantum::engine {

using cd = std::complex<double>;

// A gate description independent of the GUI Gate struct. name is one of:
// H X Y Z S T RX RY RZ CNOT CZ. For single-qubit gates target is ignored.
// angle is radians (used by RX/RY/RZ).
struct EngineGate {
    std::string name;
    int qubit = 0;
    int target = -1;
    double angle = 0.0;
};

// Statevector over n qubits. Qubit q occupies bit q of the basis index
// (qubit 0 is the least significant bit). amp[i] is the amplitude of basis
// state i, so prob(i) = norm(amp[i]).
class Statevector {
public:
    explicit Statevector(int n) : n_(n), amp_(std::size_t(1) << n, cd(0.0, 0.0)) {
        amp_[0] = cd(1.0, 0.0); // |0...0>
    }

    int num_qubits() const { return n_; }
    std::size_t dim() const { return amp_.size(); }
    const std::vector<cd>& amplitudes() const { return amp_; }

    void reset() {
        std::fill(amp_.begin(), amp_.end(), cd(0.0, 0.0));
        amp_[0] = cd(1.0, 0.0);
    }

    // Apply an arbitrary single-qubit 2x2 unitary [[m00,m01],[m10,m11]]
    // to target qubit q across every index pair (i0,i1) differing only in bit q.
    void apply_1q(int q, const cd m[2][2]) {
        const std::size_t bit = std::size_t(1) << q;
        for (std::size_t i = 0; i < amp_.size(); ++i) {
            if (i & bit) continue;            // process each pair once from the |0> member
            const std::size_t i0 = i;
            const std::size_t i1 = i | bit;
            const cd a0 = amp_[i0];
            const cd a1 = amp_[i1];
            amp_[i0] = m[0][0] * a0 + m[0][1] * a1;
            amp_[i1] = m[1][0] * a0 + m[1][1] * a1;
        }
    }

    // Controlled single-qubit unitary: apply m to target t only on indices
    // where control c is set.
    void apply_controlled_1q(int c, int t, const cd m[2][2]) {
        const std::size_t cbit = std::size_t(1) << c;
        const std::size_t tbit = std::size_t(1) << t;
        for (std::size_t i = 0; i < amp_.size(); ++i) {
            if (!(i & cbit)) continue;        // control must be 1
            if (i & tbit) continue;           // pair from target-|0> member
            const std::size_t i0 = i;
            const std::size_t i1 = i | tbit;
            const cd a0 = amp_[i0];
            const cd a1 = amp_[i1];
            amp_[i0] = m[0][0] * a0 + m[0][1] * a1;
            amp_[i1] = m[1][0] * a0 + m[1][1] * a1;
        }
    }

    void apply(const EngineGate& g) {
        const double r2 = 1.0 / std::sqrt(2.0);
        const std::string& nm = g.name;
        cd m[2][2];
        if (nm == "H") {
            m[0][0] = r2;  m[0][1] = r2;
            m[1][0] = r2;  m[1][1] = -r2;
            apply_1q(g.qubit, m);
        } else if (nm == "X") {
            m[0][0] = 0; m[0][1] = 1; m[1][0] = 1; m[1][1] = 0;
            apply_1q(g.qubit, m);
        } else if (nm == "Y") {
            m[0][0] = 0;          m[0][1] = cd(0, -1);
            m[1][0] = cd(0, 1);   m[1][1] = 0;
            apply_1q(g.qubit, m);
        } else if (nm == "Z") {
            m[0][0] = 1; m[0][1] = 0; m[1][0] = 0; m[1][1] = -1;
            apply_1q(g.qubit, m);
        } else if (nm == "S") {
            m[0][0] = 1; m[0][1] = 0; m[1][0] = 0; m[1][1] = cd(0, 1);
            apply_1q(g.qubit, m);
        } else if (nm == "T") {
            m[0][0] = 1; m[0][1] = 0;
            m[1][0] = 0; m[1][1] = std::exp(cd(0, M_PI / 4.0));
            apply_1q(g.qubit, m);
        } else if (nm == "RX") {
            const double c = std::cos(g.angle / 2.0), s = std::sin(g.angle / 2.0);
            m[0][0] = c;          m[0][1] = cd(0, -s);
            m[1][0] = cd(0, -s);  m[1][1] = c;
            apply_1q(g.qubit, m);
        } else if (nm == "RY") {
            const double c = std::cos(g.angle / 2.0), s = std::sin(g.angle / 2.0);
            m[0][0] = c;  m[0][1] = -s;
            m[1][0] = s;  m[1][1] = c;
            apply_1q(g.qubit, m);
        } else if (nm == "RZ") {
            m[0][0] = std::exp(cd(0, -g.angle / 2.0)); m[0][1] = 0;
            m[1][0] = 0; m[1][1] = std::exp(cd(0, g.angle / 2.0));
            apply_1q(g.qubit, m);
        } else if (nm == "CNOT") {
            cd x[2][2] = {{0, 1}, {1, 0}};
            apply_controlled_1q(g.qubit, g.target, x);
        } else if (nm == "CZ") {
            cd z[2][2] = {{1, 0}, {0, -1}};
            apply_controlled_1q(g.qubit, g.target, z);
        }
        // unknown names are no-ops (identity)
    }

    void run(const std::vector<EngineGate>& gates) {
        reset();
        for (const auto& g : gates) apply(g);
    }

    double total_probability() const {
        double s = 0.0;
        for (const auto& a : amp_) s += std::norm(a);
        return s;
    }

private:
    int n_;
    std::vector<cd> amp_;
};

// n-bit binary label, most-significant qubit (n-1) first. Index bit q maps to
// qubit q, matching the LSB-is-qubit-0 convention used in apply().
inline std::string basis_label(std::size_t index, int n) {
    std::string s(std::size_t(n), '0');
    for (int q = 0; q < n; ++q)
        if (index & (std::size_t(1) << q)) s[std::size_t(n - 1 - q)] = '1';
    return s;
}

// Sample `shots` measurements from the probability distribution and return
// observed (label, count) pairs in descending count order.
inline std::vector<std::pair<std::string, int>>
sample_counts(const std::vector<double>& probs, int n, int shots, std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::discrete_distribution<std::size_t> dist(probs.begin(), probs.end());
    std::vector<int> counts(probs.size(), 0);
    for (int s = 0; s < shots; ++s) counts[dist(rng)]++;
    std::vector<std::pair<std::string, int>> out;
    for (std::size_t i = 0; i < counts.size(); ++i)
        if (counts[i] > 0) out.emplace_back(basis_label(i, n), counts[i]);
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    return out;
}

// Greedy per-qubit layering: each gate goes into the earliest layer where none
// of the qubits it touches is already used. Depth = number of layers.
inline int circuit_depth(const std::vector<EngineGate>& gates, int n) {
    std::vector<int> ready(std::size_t(n), 0); // next free layer per qubit
    int depth = 0;
    for (const auto& g : gates) {
        int layer = ready[std::size_t(g.qubit)];
        if (g.target >= 0 && g.target < n)
            layer = std::max(layer, ready[std::size_t(g.target)]);
        int place = layer + 1;
        ready[std::size_t(g.qubit)] = place;
        if (g.target >= 0 && g.target < n) ready[std::size_t(g.target)] = place;
        depth = std::max(depth, place);
    }
    return depth;
}

// Fidelity from a depolarizing-only noise model: effective fidelity 1-p when
// noise is enabled with depolarizing rate p, else 1.0 (ideal).
inline double noise_fidelity(bool enabled, double depolarizing) {
    if (!enabled) return 1.0;
    double f = 1.0 - depolarizing;
    if (f < 0.0) f = 0.0;
    if (f > 1.0) f = 1.0;
    return f;
}

} // namespace straylight::quantum::engine
