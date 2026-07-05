// apps/photonics-gui/photonics_device.h
// Soft photonic device: the StrayLight photonics emulator exposed as the same
// primitives a real programmable photonic mesh chip exposes. A toy model written
// against this device runs unchanged when the backend is swapped to real silicon
// (FPGA mesh today, a photonic chip later) -- only the implementation behind
// these calls changes, not the calls.
//
// Hardware mapping of each primitive:
//   set_mzi(i, theta, phi)   -> the two thermo-optic phase shifters of MZI i
//   set_output_phase(m, p)   -> the per-mode output phase screen
//   set_loss/crosstalk/wavelength -> device impairments (the analog minutiae)
//   run(field)               -> couple a coherent input field in, read it out
//   detectors(field)         -> photodiode powers |E|^2 at the outputs
//   unitary()                -> the linear transform the device currently performs
#pragma once
#include "photonics_engine.h"
#include <array>
#include <cmath>
#include <vector>

namespace straylight::photonics {

class Device {
public:
    static constexpr int N = engine::kN;   // spatial modes (waveguides)
    using cd  = engine::cd;
    using Vec = std::array<cd, N>;

    Device() : grid_(engine::build_grid()) {}

    int num_mzis() const { int n = 0; for (auto& m : grid_) if (m.active) ++n; return n; }

    // ---- programming primitives (set the device's knobs) ----
    void set_mzi(int i, double theta, double phi) {
        int k = 0;
        for (auto& m : grid_)
            if (m.active) { if (k == i) { m.theta = theta; m.phi = phi; return; } ++k; }
    }
    void set_output_phase(int mode, double phase) { if (mode >= 0 && mode < N) out_phase_[mode] = phase; }
    void set_loss_db(double db)       { loss_db_  = db; }
    void set_crosstalk_db(double db)  { xtalk_db_ = db; }
    void set_wavelength_nm(double nm) { wl_nm_    = nm; }

    // ---- the linear transform the device currently performs ----
    engine::MatN unitary() const {
        engine::MatN U = engine::compose_mesh(grid_, ref_nm_ / wl_nm_);
        for (int r = 0; r < N; ++r) {
            cd e = std::exp(cd(0.0, out_phase_[r]));
            for (int c = 0; c < N; ++c) U.at(r, c) = e * U.at(r, c);
        }
        return U;
    }

    // ---- run / read primitives ----
    Vec run(const Vec& input) const {
        engine::MatN U = unitary();
        Vec out{};
        for (int r = 0; r < N; ++r) { cd a(0, 0); for (int c = 0; c < N; ++c) a += U.at(r, c) * input[c]; out[r] = a; }
        if (loss_db_ > 0.0) {                       // per-MZI insertion loss over mesh depth
            double atten = std::pow(10.0, -(loss_db_ * N) / 20.0);
            for (auto& x : out) x *= atten;
        }
        if (xtalk_db_ > -200.0) {                   // adjacent-mode crosstalk
            double c = std::pow(10.0, xtalk_db_ / 20.0);
            Vec l = out;
            for (int i = 0; i < N; ++i) { if (i > 0) l[i] += c * out[i-1]; if (i+1 < N) l[i] += c * out[i+1]; }
            out = l;
        }
        return out;
    }
    std::array<double, N> detectors(const Vec& input) const {
        Vec o = run(input); std::array<double, N> p{};
        for (int i = 0; i < N; ++i) p[i] = std::norm(o[i]);
        return p;
    }

private:
    std::vector<engine::GridMZI> grid_;
    std::array<double, N> out_phase_{};
    double loss_db_ = 0.0, xtalk_db_ = -200.0, wl_nm_ = 1550.0, ref_nm_ = 1550.0;
};

} // namespace straylight::photonics
