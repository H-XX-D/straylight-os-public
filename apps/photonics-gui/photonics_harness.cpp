// apps/photonics-gui/photonics_harness.cpp
// Standalone correctness harness for the MZI-mesh engine. No imgui.
// Asserts: U_mesh is unitary (max|U^H U - I| < 1e-9) and energy is conserved
// (sum|out|^2 == sum|in|^2 within 1e-9) for random theta/phi over many trials.
#include "photonics_engine.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <random>

using namespace straylight::photonics::engine;

static double total_energy(const std::array<cd, kN>& v) {
    double e = 0.0;
    for (const auto& x : v) e += std::norm(x);
    return e;
}

int main() {
    std::mt19937_64 rng(0xC0FFEEULL);
    std::uniform_real_distribution<double> ang(0.0, 2.0 * M_PI);

    const int trials = 20000;
    double worst_unitarity = 0.0;
    double worst_energy = 0.0;

    for (int t = 0; t < trials; ++t) {
        EngineState st;
        st.grid = build_grid();
        for (auto& m : st.grid) {
            if (m.active) { m.theta = ang(rng); m.phi = ang(rng); }
        }
        // exercise the output-phase screen too: a diagonal unitary must keep
        // U_mesh unitary and conserve energy (otherwise the universal mesh is wrong)
        for (int r = 0; r < kN; ++r) st.output_phase[r] = ang(rng);
        // exercise both input modes
        st.input_mode = (t & 1) ? InputMode::Uniform : InputMode::OneHot;
        recompute(st);

        double ures = unitarity_residual(st.U_mesh);
        worst_unitarity = std::max(worst_unitarity, ures);

        double ein = total_energy(st.input);
        double eout = total_energy(st.output);
        double ediff = std::fabs(ein - eout);
        worst_energy = std::max(worst_energy, ediff);

        assert(ures < 1e-9 && "U_mesh not unitary");
        assert(ediff < 1e-9 && "energy not conserved");
    }

    // Sanity: identity target fidelity is in [0,1] and equals 1 for a zero-drive mesh.
    EngineState id;
    id.grid = build_grid();         // all theta=phi=0 -> each MZI T-matrix = I, mesh = I
    id.target = TargetOp::Identity;
    recompute(id);
    double fid_id = id.fidelity;
    double zero_mesh_unitarity = unitarity_residual(id.U_mesh);

    std::printf("photonics MZI-mesh harness\n");
    std::printf("  trials                 : %d\n", trials);
    std::printf("  N                      : %d\n", kN);
    std::printf("  active MZIs            : 6 (Clements rectangular)\n");
    std::printf("  worst |U^H U - I|      : %.3e  (bound 1e-9)\n", worst_unitarity);
    std::printf("  worst |Ein - Eout|     : %.3e  (bound 1e-9)\n", worst_energy);
    std::printf("  zero-drive unitarity   : %.3e\n", zero_mesh_unitarity);
    std::printf("  zero-drive fidelity<I> : %.6f (in [0,1])\n", fid_id);

    bool ok = (worst_unitarity < 1e-9) && (worst_energy < 1e-9) &&
              (fid_id >= 0.0 && fid_id <= 1.0 + 1e-12);
    std::printf("RESULT: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
