// apps/quantum-gui/quantum_harness.cpp
// Standalone correctness harness for the statevector engine. No imgui, no GUI.
// Builds with: g++ -std=c++20 quantum_harness.cpp -o quantum_harness
//
// Invariants checked:
//   1. Bell state (H q0; CNOT q0,q1) on 2 qubits:
//        sum(probabilities) == 1            (within 1e-9)
//        prob(|00>) ~= 0.5, prob(|11>) ~= 0.5
//        prob(|01>) ~= 0,   prob(|10>) ~= 0
//   2. Norm preserved (within 1e-9) after a random 20-gate sequence on 4 qubits.
#include "quantum_engine.h"
#include <cassert>
#include <cstdio>
#include <cmath>
#include <random>
#include <vector>
#include <string>

using namespace straylight::quantum::engine;

static bool close(double a, double b, double tol = 1e-9) {
    return std::fabs(a - b) <= tol;
}

int main() {
    int failures = 0;
    auto check = [&](bool cond, const char* msg) {
        std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", msg);
        if (!cond) ++failures;
    };

    // ---- Invariant 1: Bell state ----
    std::printf("Invariant 1: Bell state (H q0; CNOT q0,q1)\n");
    {
        Statevector sv(2);
        std::vector<EngineGate> bell = {
            {"H", 0, -1, 0.0},
            {"CNOT", 0, 1, 0.0},
        };
        sv.run(bell);
        const auto& amp = sv.amplitudes();

        std::vector<double> p(amp.size());
        double sum = 0.0;
        for (std::size_t i = 0; i < amp.size(); ++i) { p[i] = std::norm(amp[i]); sum += p[i]; }

        // index bit0 = qubit0, bit1 = qubit1. |00>=0, |01>(q0=1)=1, |10>(q1=1)=2, |11>=3
        std::printf("  probabilities: |00>=%.6f |01>=%.6f |10>=%.6f |11>=%.6f  sum=%.12f\n",
                    p[0], p[1], p[2], p[3], sum);

        check(close(sum, 1.0), "sum(probabilities) == 1 (within 1e-9)");
        check(close(p[0], 0.5), "prob(|00>) ~= 0.5");
        check(close(p[3], 0.5), "prob(|11>) ~= 0.5");
        check(close(p[1], 0.0), "prob(|01>) ~= 0");
        check(close(p[2], 0.0), "prob(|10>) ~= 0");

        // basis labels sanity
        check(basis_label(0, 2) == "00", "basis_label(0,2)==\"00\"");
        check(basis_label(3, 2) == "11", "basis_label(3,2)==\"11\"");
        check(basis_label(1, 2) == "01", "basis_label(1,2)==\"01\" (qubit0 is LSB)");
        check(basis_label(2, 2) == "10", "basis_label(2,2)==\"10\"");
    }

    // ---- Invariant 2: norm preserved after random 20-gate sequence ----
    std::printf("Invariant 2: norm preserved after random 20-gate sequence (4 qubits)\n");
    {
        const int n = 4;
        std::mt19937 rng(12345u);
        const char* singles[] = {"H","X","Y","Z","S","T","RX","RY","RZ"};
        const char* twos[]    = {"CNOT","CZ"};
        std::uniform_int_distribution<int> coin(0, 4);       // ~1/5 two-qubit
        std::uniform_int_distribution<int> qd(0, n - 1);
        std::uniform_int_distribution<int> sidx(0, 8);
        std::uniform_int_distribution<int> tidx(0, 1);
        std::uniform_real_distribution<double> ang(0.0, 2.0 * M_PI);

        std::vector<EngineGate> seq;
        while ((int)seq.size() < 20) {
            if (coin(rng) == 0) {
                int c = qd(rng), t = qd(rng);
                if (c == t) continue;
                seq.push_back({twos[tidx(rng)], c, t, 0.0});
            } else {
                seq.push_back({singles[sidx(rng)], qd(rng), -1, ang(rng)});
            }
        }

        Statevector sv(n);
        sv.run(seq);
        double total = sv.total_probability();
        std::printf("  20 gates applied; total probability = %.15f\n", total);
        check(close(total, 1.0), "norm preserved: sum(|amp|^2) == 1 (within 1e-9)");
    }

    std::printf("\n%s (%d failure%s)\n",
                failures == 0 ? "ALL INVARIANTS HOLD" : "INVARIANT VIOLATION",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
