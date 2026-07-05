// apps/photonics-gui/photonics_engine.h
// Real MZI-mesh photonic simulation engine (no imgui, no photonic hardware).
//
// Each MZI is a 2x2 unitary
//     U(theta, phi) = Rphase(phi) * BS * Rphase(theta) * BS
// with
//     BS        = (1/sqrt(2)) [[1, i], [i, 1]]
//     Rphase(x) = diag(e^{i x}, 1)
//
// The mesh is a Clements rectangular arrangement of MZIs composed into an
// NxN complex unitary U_mesh. An input field is propagated through U_mesh to
// produce the output field. All struct fields the GUI renders are computed
// here; nothing is fabricated.
#pragma once

#include <array>
#include <complex>
#include <cmath>
#include <cstddef>
#include <vector>

namespace straylight::photonics::engine {

using cd = std::complex<double>;

constexpr int kN = 4;             // mesh dimension (4x4)
constexpr int kGrid = kN * kN;    // 16 grid cells drawn by the GUI

// A single 2x2 unitary on the (0,0),(0,1),(1,0),(1,1) entries.
struct Mat2 {
    cd m00, m01, m10, m11;
};

// Beamsplitter BS = (1/sqrt(2)) [[1, i], [i, 1]].
inline Mat2 bs() {
    const double s = 1.0 / std::sqrt(2.0);
    return Mat2{ cd(s, 0.0), cd(0.0, s),
                 cd(0.0, s), cd(s, 0.0) };
}

// 2x2 matrix product A*B.
inline Mat2 mul2(const Mat2& a, const Mat2& b) {
    return Mat2{
        a.m00 * b.m00 + a.m01 * b.m10,
        a.m00 * b.m01 + a.m01 * b.m11,
        a.m10 * b.m00 + a.m11 * b.m10,
        a.m10 * b.m01 + a.m11 * b.m11,
    };
}

// Rphase(x) = diag(e^{i x}, 1) as a 2x2.
inline Mat2 rphase(double x) {
    return Mat2{ std::exp(cd(0.0, x)), cd(0.0, 0.0),
                 cd(0.0, 0.0),         cd(1.0, 0.0) };
}

// Universal MZI: U(theta,phi) = [[e^{i phi} cos(theta/2), -sin(theta/2)],
//                                 [e^{i phi} sin(theta/2),  cos(theta/2)]].
// theta=pi/2 is a 50/50 splitter, theta=0 is pass-through. With the Clements
// schedule + an output phase screen this mesh is universal (implements any NxN
// unitary -- DFT, Hadamard...), which the prior BS*Rphase*BS*Rphase form was not.
inline Mat2 mzi_unitary(double theta, double phi) {
    const double ct = std::cos(theta / 2.0), st = std::sin(theta / 2.0);
    const cd e = std::exp(cd(0.0, phi));
    return Mat2{ e * ct, cd(-st, 0.0),
                 e * st, cd(ct, 0.0) };
}

// NxN dense complex matrix, row-major.
struct MatN {
    std::array<cd, kN * kN> a{};
    cd& at(int r, int c) { return a[r * kN + c]; }
    cd  at(int r, int c) const { return a[r * kN + c]; }

    static MatN identity() {
        MatN m;
        for (int i = 0; i < kN; ++i) m.at(i, i) = cd(1.0, 0.0);
        return m;
    }
};

// Embed a 2x2 unitary acting on modes (p, p+1) and left-multiply: out = E * in.
// E is identity except on rows/cols p and p+1.
inline MatN apply_mzi_left(const MatN& in, const Mat2& u, int p) {
    MatN out = in;
    for (int c = 0; c < kN; ++c) {
        cd a = in.at(p, c);
        cd b = in.at(p + 1, c);
        out.at(p, c)     = u.m00 * a + u.m01 * b;
        out.at(p + 1, c) = u.m10 * a + u.m11 * b;
    }
    return out;
}

inline MatN matmulN(const MatN& A, const MatN& B) {
    MatN C;
    for (int i = 0; i < kN; ++i)
        for (int k = 0; k < kN; ++k) {
            cd aik = A.at(i, k);
            if (aik == cd(0.0, 0.0)) continue;
            for (int j = 0; j < kN; ++j)
                C.at(i, j) += aik * B.at(k, j);
        }
    return C;
}

// Conjugate transpose.
inline MatN daggerN(const MatN& A) {
    MatN H;
    for (int i = 0; i < kN; ++i)
        for (int j = 0; j < kN; ++j)
            H.at(i, j) = std::conj(A.at(j, i));
    return H;
}

// One programmable MZI placed on the drawing grid. Grid cells that are not part
// of the Clements schedule are passthrough (identity) and excluded from U_mesh.
struct GridMZI {
    int  id = 0;        // 0..15, row*4+col
    int  row = 0;
    int  col = 0;
    bool active = false;   // participates in the Clements unitary
    int  mode_pair = 0;    // top mode p of the (p, p+1) pair it acts on
    int  order = 0;        // application order within U_mesh (0 first)
    double theta = 0.0;
    double phi = 0.0;
    double transmission = 0.0;  // |U_mzi[0][0]|^2
};

// Clements rectangular schedule for N=4: six MZIs.
// Column index selects the alternating mode-pair pattern of a rectangular mesh:
//   even columns use pairs (0,1) and (2,3); odd columns use pair (1,2).
// We place the six active MZIs on grid cells so every active cell maps to a
// distinct (order, mode_pair). The remaining grid cells are passthrough.
//
// Layout (col -> mode pairs), N=4 needs 4 columns of a rectangular mesh:
//   col0: pairs (0,1),(2,3)   col1: pair (1,2)
//   col2: pairs (0,1),(2,3)   col3: pair (1,2)
// That is 2+1+2+1 = 6 MZIs, matching N(N-1)/2.
struct ClementsSlot { int row; int col; int mode_pair; int order; };

inline const std::array<ClementsSlot, 6>& clements_slots() {
    // row/col are GUI grid positions chosen so each MZI sits on a visible,
    // non-overlapping cell; mode_pair/order define the actual unitary.
    static const std::array<ClementsSlot, 6> slots = {{
        { 0, 0, 0, 0 },  // col0 pair (0,1)
        { 2, 0, 2, 1 },  // col0 pair (2,3)
        { 1, 1, 1, 2 },  // col1 pair (1,2)
        { 0, 2, 0, 3 },  // col2 pair (0,1)
        { 2, 2, 2, 4 },  // col2 pair (2,3)
        { 1, 3, 1, 5 },  // col3 pair (1,2)
    }};
    return slots;
}

enum class InputMode { OneHot, Uniform };
enum class TargetOp { Identity, DFT, Hadamard };

struct EngineState {
    std::vector<GridMZI> grid;           // 16 cells, row-major
    MatN U_mesh = MatN::identity();
    std::array<cd, kN> input{};
    std::array<cd, kN> output{};
    InputMode input_mode = InputMode::OneHot;
    TargetOp  target = TargetOp::Identity;

    // Output phase screen (one phase per mode). Makes the mesh universal so a
    // decomposition can implement any target unitary. All zero by default.
    std::array<double, kN> output_phase{};

    double fidelity = 0.0;   // |trace(U_target^H * U_mesh)| / N
    double noise_floor_dbm = -60.0;  // modeled fixed noise floor

    // Physical device parameters (driven by the Calibrate tab). Defaults are the
    // ideal device: lossless, no crosstalk, on-design wavelength. With the
    // defaults, U_mesh stays exactly unitary and energy is conserved, so the
    // 20k-trial correctness harness still holds. Non-ideal values model real
    // hardware impairments and are reflected in the output field / detectors.
    double insertion_loss_db = 0.0;     // per-MZI insertion loss (dB)
    double crosstalk_db = -200.0;       // adjacent-mode power crosstalk (dB); <=-200 => off
    double wavelength_nm = 1550.0;      // operating wavelength
    double ref_wavelength_nm = 1550.0;  // design wavelength (phase set point)
};

// Build the 16-cell grid with the six active Clements MZIs.
inline std::vector<GridMZI> build_grid() {
    std::vector<GridMZI> g(kGrid);
    for (int r = 0; r < kN; ++r)
        for (int c = 0; c < kN; ++c) {
            GridMZI m;
            m.id = r * kN + c;
            m.row = r;
            m.col = c;
            m.active = false;
            g[m.id] = m;
        }
    for (const auto& s : clements_slots()) {
        int id = s.row * kN + s.col;
        g[id].active = true;
        g[id].mode_pair = s.mode_pair;
        g[id].order = s.order;
    }
    return g;
}

// Compose U_mesh from the active MZIs in application order. phase_scale models
// chromatic dispersion of the phase shifters: accumulated phase scales as
// (lambda_ref / lambda), so phase_scale = 1.0 at the design wavelength. Scaling
// both phases preserves unitarity, so U_mesh stays unitary at any wavelength.
inline MatN compose_mesh(const std::vector<GridMZI>& grid, double phase_scale = 1.0) {
    // Collect active MZIs sorted by order.
    std::array<const GridMZI*, 6> active{};
    int n = 0;
    for (const auto& m : grid)
        if (m.active && n < 6) active[n++] = &m;
    // selection sort by order (n is tiny)
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
            if (active[j]->order < active[i]->order) std::swap(active[i], active[j]);

    MatN U = MatN::identity();
    for (int i = 0; i < n; ++i) {
        Mat2 u = mzi_unitary(active[i]->theta * phase_scale, active[i]->phi * phase_scale);
        U = apply_mzi_left(U, u, active[i]->mode_pair);
    }
    return U;
}

inline MatN target_matrix(TargetOp t) {
    if (t == TargetOp::DFT) {
        MatN m;
        const double s = 1.0 / std::sqrt((double)kN);
        for (int r = 0; r < kN; ++r)
            for (int c = 0; c < kN; ++c) {
                double ang = -2.0 * M_PI * r * c / (double)kN;
                m.at(r, c) = s * std::exp(cd(0.0, ang));
            }
        return m;
    }
    if (t == TargetOp::Hadamard) {
        // 4x4 Walsh-Hadamard, normalized.
        static const int H4[4][4] = {
            { 1,  1,  1,  1},
            { 1, -1,  1, -1},
            { 1,  1, -1, -1},
            { 1, -1, -1,  1},
        };
        MatN m;
        const double s = 0.5;  // 1/sqrt(4)
        for (int r = 0; r < kN; ++r)
            for (int c = 0; c < kN; ++c)
                m.at(r, c) = cd(s * H4[r][c], 0.0);
        return m;
    }
    return MatN::identity();
}

inline double fidelity_of(const MatN& U_mesh, TargetOp t) {
    MatN tgt = target_matrix(t);
    MatN H = daggerN(tgt);
    cd tr(0.0, 0.0);
    // trace(H * U_mesh) = sum_i (H*U)_{ii} = sum_i sum_k H_{ik} U_{ki}
    for (int i = 0; i < kN; ++i)
        for (int k = 0; k < kN; ++k)
            tr += H.at(i, k) * U_mesh.at(k, i);
    return std::abs(tr) / (double)kN;
}

inline std::array<cd, kN> make_input(InputMode mode) {
    std::array<cd, kN> in{};
    if (mode == InputMode::OneHot) {
        in[0] = cd(1.0, 0.0);
    } else {
        const double s = 1.0 / std::sqrt((double)kN);
        for (int i = 0; i < kN; ++i) in[i] = cd(s, 0.0);
    }
    return in;
}

inline std::array<cd, kN> propagate(const MatN& U, const std::array<cd, kN>& in) {
    std::array<cd, kN> out{};
    for (int r = 0; r < kN; ++r) {
        cd acc(0.0, 0.0);
        for (int c = 0; c < kN; ++c) acc += U.at(r, c) * in[c];
        out[r] = acc;
    }
    return out;
}

// Recompute U_mesh, per-MZI transmission, output field, and fidelity.
inline void recompute(EngineState& st) {
    // Chromatic phase scaling: phase ~ 1/lambda, =1 at the design wavelength.
    const double phase_scale = (st.wavelength_nm > 0.0)
        ? st.ref_wavelength_nm / st.wavelength_nm : 1.0;

    st.U_mesh = compose_mesh(st.grid, phase_scale);
    // Output phase screen: left-multiply row r by e^{i*output_phase[r]}. This is
    // the second half of a universal Clements mesh; with it the mesh can realize
    // any NxN unitary. Diagonal-unitary => U_mesh stays unitary (harness holds).
    for (int r = 0; r < kN; ++r) {
        const cd e = std::exp(cd(0.0, st.output_phase[r] * phase_scale));
        for (int c = 0; c < kN; ++c) st.U_mesh.at(r, c) = e * st.U_mesh.at(r, c);
    }
    for (auto& m : st.grid) {
        if (m.active) {
            Mat2 u = mzi_unitary(m.theta * phase_scale, m.phi * phase_scale);
            m.transmission = std::norm(u.m00);  // |U_mzi[0][0]|^2
        } else {
            m.transmission = 1.0;  // passthrough
        }
    }
    st.input = make_input(st.input_mode);

    // Ideal (unitary) propagation through the mesh.
    std::array<cd, kN> field = propagate(st.U_mesh, st.input);

    // Insertion loss: a signal traverses the mesh depth (= kN stages for an
    // N-mode rectangular Clements mesh). Total path loss = insertion_loss_db*kN,
    // applied as amplitude factor 10^(-loss_dB/20). loss=0 => factor 1.
    if (st.insertion_loss_db > 0.0) {
        const double atten = std::pow(10.0, -(st.insertion_loss_db * kN) / 20.0);
        for (auto& x : field) x *= atten;
    }

    // Adjacent-mode crosstalk: each detector picks up a coherent fraction of its
    // neighbours' fields, amplitude coupling c = 10^(crosstalk_dB/20).
    // crosstalk_db <= -200 dB => effectively off (c ~ 0).
    if (st.crosstalk_db > -200.0) {
        const double c = std::pow(10.0, st.crosstalk_db / 20.0);
        std::array<cd, kN> leaked = field;
        for (int i = 0; i < kN; ++i) {
            if (i > 0)      leaked[i] += c * field[i - 1];
            if (i + 1 < kN) leaked[i] += c * field[i + 1];
        }
        field = leaked;
    }

    st.output = field;
    st.fidelity = fidelity_of(st.U_mesh, st.target);
}

// Detector power in dBm from output mode magnitude: 10*log10(|out_i|^2 + 1e-12).
inline double power_dbm(cd out_i) {
    return 10.0 * std::log10(std::norm(out_i) + 1e-12);
}

// Normalized heater drive for grid cell k: theta/(2pi) for active cells, the
// drive that the renderer maps to a thermal color. Inactive cells read 0.
inline double heat_norm(const GridMZI& m) {
    if (!m.active) return 0.0;
    double v = m.theta / (2.0 * M_PI);
    v -= std::floor(v);   // wrap to [0,1)
    return v;
}

// Max abs element of (U^H * U - I); 0 for a perfect unitary.
inline double unitarity_residual(const MatN& U) {
    MatN H = daggerN(U);
    MatN P = matmulN(H, U);
    double worst = 0.0;
    for (int i = 0; i < kN; ++i)
        for (int j = 0; j < kN; ++j) {
            cd e = P.at(i, j) - (i == j ? cd(1.0, 0.0) : cd(0.0, 0.0));
            worst = std::max(worst, std::abs(e));
        }
    return worst;
}

} // namespace straylight::photonics::engine
