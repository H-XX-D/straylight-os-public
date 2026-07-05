// apps/photonics-gui/photonics_panel.cpp
#include "photonics_panel.h"
#include "photonics_decompose_apply.h"
#include <imgui.h>
#include <cmath>
#include <cstdio>
#include <algorithm>

namespace straylight::photonics {

static constexpr ImVec4 kCyan     = {0.098f, 0.906f, 1.000f, 1.0f};
static constexpr ImVec4 kPurple   = {0.545f, 0.361f, 0.965f, 1.0f};
static constexpr ImVec4 kGold     = {0.957f, 0.722f, 0.271f, 1.0f};
static constexpr ImVec4 kBgPanel  = {0.035f, 0.055f, 0.110f, 0.90f};
static constexpr ImVec4 kMuted    = {1.0f, 1.0f, 1.0f, 0.55f};
static constexpr ImVec4 kMuted2   = {1.0f, 1.0f, 1.0f, 0.30f};
static constexpr ImVec4 kSuccess  = {0.133f, 0.773f, 0.447f, 1.0f};
static constexpr ImVec4 kDanger   = {1.0f, 0.298f, 0.416f, 1.0f};

static ImU32 ToU32(ImVec4 v) {
    return IM_COL32(static_cast<int>(v.x*255), static_cast<int>(v.y*255),
                    static_cast<int>(v.z*255), static_cast<int>(v.w*255));
}

static engine::TargetOp target_for_index(int i) {
    switch (i) {
        case 0:  return engine::TargetOp::DFT;
        case 2:  return engine::TargetOp::Hadamard;
        default: return engine::TargetOp::Identity;
    }
}

void PhotonicsState::init() {
    // 4x4 Clements MZI mesh — fields below are derived from the real engine,
    // never fabricated.
    mesh.rows = 4;
    mesh.cols = 4;
    mesh.topology = "Clements";
    mesh.wavelength_nm = 1550.0f;

    eng.grid = engine::build_grid();
    eng.input_mode = engine::InputMode::OneHot;
    eng.target = engine::TargetOp::Identity;

    // Seed the active MZIs with a deterministic, reproducible drive pattern
    // (a smooth ramp over the heater settings) so the mesh is a concrete,
    // non-trivial unitary. No rand().
    int k = 0;
    for (auto& m : eng.grid) {
        if (m.active) {
            m.theta = 0.30 + 0.50 * static_cast<double>(k);
            m.phi   = 0.15 + 0.35 * static_cast<double>(k);
            ++k;
        }
    }

    // Build the GUI MZI list (16 grid cells, row-major) mirroring the engine.
    mesh.mzis.clear();
    for (const auto& gm : eng.grid) {
        MZI mzi;
        mzi.id    = gm.id;
        mzi.x     = static_cast<float>(gm.col);
        mzi.y     = static_cast<float>(gm.row);
        mzi.active = gm.active;
        mzi.theta = static_cast<float>(gm.theta);
        mzi.phi   = static_cast<float>(gm.phi);
        mzi.transmission = 0.0f;
        mesh.mzis.push_back(mzi);
    }

    // Four detectors, one per output mode.
    detectors.clear();
    for (int i = 0; i < engine::kN; ++i) {
        DetectorState det;
        det.id = i;
        det.power_dbm = -60.0f;
        det.snr_db = 0.0f;
        det.saturated = false;
        det.history_offset = 0;
        for (int h = 0; h < 128; ++h) det.history[h] = -60.0f;
        detectors.push_back(det);
    }

    // Target unitary ops; fidelity/error/snr filled by the engine per target.
    ops.clear();
    ops.push_back({"DFT-4",      {}, 0.0f, 0.0f, 0.0f});
    ops.push_back({"Identity",   {}, 0.0f, 0.0f, 0.0f});
    ops.push_back({"Hadamard-4", {}, 0.0f, 0.0f, 0.0f});

    input_mode = 0;
    selected_target = 1;
    dirty_ = true;
    recompute();
}

void PhotonicsState::set_mzi(int grid_id, float theta, float phi) {
    if (grid_id < 0 || grid_id >= static_cast<int>(eng.grid.size())) return;
    if (!eng.grid[grid_id].active) return;   // only programmable crossings tune
    eng.grid[grid_id].theta = theta;
    eng.grid[grid_id].phi   = phi;
    dirty_ = true;
}

void PhotonicsState::program_target() {
    // Synthesize the selected target into the mesh. Identity is trivial (bar
    // state); DFT/Hadamard call the StrayLight Solver winch via the decompose
    // tool, then we apply the phases and recompute. The displayed fidelity is
    // the engine's own |tr(U_target^H U_mesh)|/N after recompute, not the
    // tool's claim -- the engine has the last word.
    engine::TargetOp t = target_for_index(selected_target);
    if (t == engine::TargetOp::Identity) {
        for (auto& m : eng.grid) if (m.active) { m.theta = 0.0; m.phi = 0.0; }
        for (int r = 0; r < engine::kN; ++r) eng.output_phase[r] = 0.0;
        dirty_ = true; recompute();
        synth_ok = true;
        synth_msg = "Programmed Identity (bar-state mesh, no synthesis needed)";
        return;
    }
    const std::string tname = (t == engine::TargetOp::DFT) ? "dft" : "hadamard";
    std::string msg;
    bool ok = sl_decompose_into_engine(eng, tname, msg);
    dirty_ = true; recompute();
    if (ok) {
        char b[176];
        std::snprintf(b, sizeof(b),
            "Programmed %s: process fidelity %.4f%% (solver.winch, 6 MZIs + 4 phases)",
            tname.c_str(), eng.fidelity * 100.0);
        synth_msg = b;
        synth_ok = eng.fidelity > 0.999;
    } else {
        synth_msg = msg;     // surface the tool/parse error verbatim
        synth_ok = false;
    }
}

void PhotonicsState::sync_from_engine() {
    // Per-MZI transmission and heater drive (heat_map) from the engine.
    for (auto& mzi : mesh.mzis) {
        const auto& gm = eng.grid[mzi.id];
        mzi.theta = static_cast<float>(gm.theta);
        mzi.phi   = static_cast<float>(gm.phi);
        mzi.active = gm.active;
        mzi.transmission = static_cast<float>(gm.transmission);
        // heat_map is drawn as a temperature in [~25,95] C; map the normalized
        // heater phase drive (theta/2pi in [0,1)) onto that range. Inactive
        // passthrough cells idle near ambient.
        double hn = engine::heat_norm(gm);
        heat_map[mzi.id] = gm.active
            ? static_cast<float>(25.0 + 70.0 * hn)
            : 25.0f;
    }

    // Detector powers and SNR from the real output field.
    const double noise = eng.noise_floor_dbm;  // modeled fixed noise floor
    for (int i = 0; i < static_cast<int>(detectors.size()) && i < engine::kN; ++i) {
        auto& det = detectors[i];
        double p = engine::power_dbm(eng.output[i]);
        det.power_dbm = static_cast<float>(p);
        det.snr_db = static_cast<float>(p - noise);   // modeled SNR vs floor
        det.saturated = (p > -3.0);
        det.history[det.history_offset] = static_cast<float>(p);
        det.history_offset = (det.history_offset + 1) % 128;
    }

    // Op fidelity/error/snr per target via the engine.
    for (int i = 0; i < static_cast<int>(ops.size()); ++i) {
        double fid = engine::fidelity_of(eng.U_mesh, target_for_index(i));
        ops[i].fidelity = static_cast<float>(fid);
        // error_db = 10*log10(1 - fidelity); modeled infidelity in dB.
        double infid = std::max(1.0 - fid, 1e-12);
        ops[i].error_db = static_cast<float>(10.0 * std::log10(infid));
        // modeled SNR vs the fixed noise floor using mean output power.
        double mean_p = 0.0;
        for (int m = 0; m < engine::kN; ++m) mean_p += engine::power_dbm(eng.output[m]);
        mean_p /= engine::kN;
        ops[i].snr_db = static_cast<float>(mean_p - eng.noise_floor_dbm);
    }
}

void PhotonicsState::recompute() {
    eng.input_mode = (input_mode == 1) ? engine::InputMode::Uniform
                                       : engine::InputMode::OneHot;
    eng.target = target_for_index(selected_target);
    // Feed the Calibrate-tab device parameters into the engine so insertion
    // loss, crosstalk, and wavelength actually affect the simulated output.
    eng.insertion_loss_db = static_cast<double>(mesh.insertion_loss_db);
    eng.crosstalk_db      = static_cast<double>(mesh.crosstalk_db);
    eng.wavelength_nm     = static_cast<double>(mesh.wavelength_nm);
    engine::recompute(eng);
    sync_from_engine();
    dirty_ = false;
}

void PhotonicsState::maybe_update() {
    // Render-loop update (health-gui / flux-gui pattern): recompute when the
    // mesh has been retuned, and refresh the detector time-series on a cadence.
    double now = ImGui::GetTime();
    if (dirty_) { recompute(); last_update_ = now; return; }
    if (now - last_update_ >= 0.10) {   // ~10 Hz history sampling
        sync_from_engine();
        last_update_ = now;
    }
}

void PhotonicsPanel::render_mesh_tab() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::BeginChild("MeshPanel", ImVec2(0.0f, 0.0f), true);

    ImGui::TextColored(kCyan, "PHOTONIC MESH — %s  %dx%d",
        state_.mesh.topology.c_str(), state_.mesh.rows, state_.mesh.cols);
    ImGui::Separator();
    ImGui::Spacing();

    // Topology label (Clements rectangular is the modeled arrangement).
    const char* topologies[] = {"Clements", "Reck", "Butterfly"};
    for (int i = 0; i < 3; ++i) {
        if (i > 0) ImGui::SameLine();
        bool sel = (state_.mesh.topology == topologies[i]);
        if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.098f, 0.906f, 1.000f, 0.25f});
        if (ImGui::Button(topologies[i], ImVec2(90.0f, 28.0f)))
            state_.mesh.topology = topologies[i];
        if (sel) ImGui::PopStyleColor();
    }
    ImGui::SameLine(0.0f, 20.0f);
    ImGui::TextColored(kMuted, "λ = %.0f nm", state_.mesh.wavelength_nm);

    // Input field selector — drives the real propagation.
    ImGui::SameLine(0.0f, 20.0f);
    ImGui::TextColored(kMuted, "input:");
    ImGui::SameLine();
    const char* in_names[] = {"one-hot e0", "uniform"};
    for (int i = 0; i < 2; ++i) {
        if (i > 0) ImGui::SameLine();
        bool sel = (state_.input_mode == i);
        if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.545f, 0.361f, 0.965f, 0.30f});
        if (ImGui::Button(in_names[i], ImVec2(86.0f, 24.0f))) {
            state_.input_mode = i;
            state_.dirty_ = true;
        }
        if (sel) ImGui::PopStyleColor();
    }

    ImGui::Spacing();

    // Draw MZI grid
    ImDrawList* dl   = ImGui::GetWindowDrawList();
    ImVec2 origin    = ImGui::GetCursorScreenPos();
    float cell_w     = 68.0f;
    float cell_h     = 58.0f;
    float pad         = 6.0f;

    static int   edit_id = -1;
    static float edit_theta = 0.0f;
    static float edit_phi   = 0.0f;

    for (const auto& mzi : state_.mesh.mzis) {
        float bx = origin.x + mzi.x * cell_w + pad;
        float by = origin.y + mzi.y * cell_h + pad;
        float bw = cell_w - pad * 2.0f;
        float bh = cell_h - pad * 2.0f;

        // Thermal color
        float temp = state_.heat_map[mzi.id];
        ImVec4 therm_col;
        if (temp > 70.0f) {
            float t = (temp - 70.0f) / 20.0f;
            therm_col = {kGold.x, kGold.y * (1.0f - t * 0.5f), kGold.z * 0.1f, 0.85f};
        } else if (temp > 50.0f) {
            float t = (temp - 50.0f) / 20.0f;
            therm_col = {kPurple.x + t * 0.3f, kPurple.y + t * 0.3f, kPurple.z, 0.75f};
        } else {
            therm_col = {kCyan.x * 0.3f, kCyan.y * 0.4f, kCyan.z * 0.6f, 0.65f};
        }

        // Dim passthrough (non-Clements) cells.
        if (!mzi.active) therm_col.w *= 0.35f;

        dl->AddRectFilled({bx, by}, {bx + bw, by + bh}, ToU32(therm_col), 6.0f);
        dl->AddRect({bx, by}, {bx + bw, by + bh}, ToU32(kCyan), 6.0f, 0, 1.0f);

        // Rotation indicator line for theta
        float cx2 = bx + bw / 2.0f;
        float cy2 = by + bh / 2.0f;
        float len = 10.0f;
        float lx = cx2 + len * cosf(mzi.theta);
        float ly = cy2 + len * sinf(mzi.theta);
        dl->AddLine({cx2, cy2}, {lx, ly}, IM_COL32(255, 255, 255, 200), 2.0f);

        char label[16];
        std::snprintf(label, sizeof(label), "M%d", mzi.id);
        dl->AddText({bx + 3.0f, by + 3.0f}, IM_COL32(180,200,220,200), label);

        char temp_label[8];
        std::snprintf(temp_label, sizeof(temp_label), "%.0f°", temp);
        dl->AddText({bx + 3.0f, by + bh - 14.0f}, IM_COL32(220,200,140,200), temp_label);

        // Click an active crossing to tune theta/phi (recomputes the mesh).
        if (mzi.active &&
            ImGui::IsMouseHoveringRect({bx,by}, {bx+bw, by+bh}) && ImGui::IsMouseClicked(0)) {
            edit_id = mzi.id;
            edit_theta = mzi.theta;
            edit_phi   = static_cast<float>(state_.eng.grid[mzi.id].phi);
            ImGui::OpenPopup("edit_mzi");
        }
    }

    ImGui::Dummy(ImVec2(static_cast<float>(state_.mesh.cols) * cell_w,
                         static_cast<float>(state_.mesh.rows) * cell_h));

    if (ImGui::BeginPopup("edit_mzi")) {
        ImGui::TextColored(kCyan, "Edit MZI M%d", edit_id);
        bool changed = false;
        changed |= ImGui::SliderFloat("θ (theta)", &edit_theta, 0.0f, 6.2832f, "%.3f");
        changed |= ImGui::SliderFloat("φ (phi)",   &edit_phi,   0.0f, 6.2832f, "%.3f");
        if (changed && edit_id >= 0) state_.set_mzi(edit_id, edit_theta, edit_phi);
        if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void PhotonicsPanel::render_detectors_tab() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::BeginChild("DetectorsPanel", ImVec2(0.0f, 0.0f), true);

    ImGui::TextColored(kPurple, "PHOTODETECTORS  (modeled noise floor %.0f dBm)",
        state_.eng.noise_floor_dbm);
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.098f, 0.906f, 1.000f, 0.12f});
    if (ImGui::Button("Recompute", ImVec2(100.0f, 28.0f))) {
        state_.dirty_ = true;
    }
    ImGui::PopStyleColor();

    ImGui::Spacing();

    float avail_w = ImGui::GetContentRegionAvail().x;
    float card_w  = (avail_w - 12.0f) / 2.0f;

    for (int i = 0; i < static_cast<int>(state_.detectors.size()); ++i) {
        auto& det = state_.detectors[i];

        if (i % 2 != 0) ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_ChildBg,
            det.saturated ? ImVec4{0.15f, 0.03f, 0.03f, 1.0f} : ImVec4{0.030f, 0.048f, 0.095f, 1.0f});
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);

        char card_id[32];
        std::snprintf(card_id, sizeof(card_id), "det_card_%d", i);
        ImGui::BeginChild(card_id, ImVec2(card_w, 140.0f), true);

        ImGui::TextColored(kCyan, "Detector %d", det.id);
        if (det.saturated) {
            ImGui::SameLine();
            ImGui::TextColored(kDanger, " [SATURATED]");
        }

        ImGui::TextColored(kMuted, "Power:");
        ImGui::SameLine();
        ImVec4 pwr_col = det.power_dbm > -10.0f ? kDanger : kSuccess;
        ImGui::TextColored(pwr_col, "%.1f dBm", det.power_dbm);

        // Power bar: normalize -60 to 0 dBm
        float pwr_norm = (det.power_dbm + 60.0f) / 60.0f;
        pwr_norm = std::clamp(pwr_norm, 0.0f, 1.0f);
        char pwr_label[16];
        std::snprintf(pwr_label, sizeof(pwr_label), "%.1f dBm", det.power_dbm);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, det.saturated ? kDanger : kSuccess);
        ImGui::ProgressBar(pwr_norm, ImVec2(-1.0f, 6.0f), "");
        ImGui::PopStyleColor();

        ImGui::TextColored(kMuted, "SNR: ");
        ImGui::SameLine();
        ImGui::TextColored(det.snr_db > 20.0f ? kSuccess : kGold, "%.1f dB", det.snr_db);

        ImGui::PushStyleColor(ImGuiCol_PlotLines, kPurple);
        ImGui::PlotLines("##det_hist", det.history, 128, det.history_offset,
            nullptr, -60.0f, 0.0f, ImVec2(-1.0f, 30.0f));
        ImGui::PopStyleColor();

        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void PhotonicsPanel::render_matrix_tab() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::BeginChild("MatrixPanel", ImVec2(0.0f, 0.0f), true);

    ImGui::TextColored(kGold, "REALIZED UNITARY  U_mesh  (target: %s)",
        state_.ops[state_.selected_target].name.c_str());
    ImGui::Separator();
    ImGui::Spacing();

    // Target operation selector — switching recomputes the fidelity vs U_mesh.
    for (int i = 0; i < static_cast<int>(state_.ops.size()); ++i) {
        if (i > 0) ImGui::SameLine();
        char btn_id[32];
        std::snprintf(btn_id, sizeof(btn_id), "%s##op%d", state_.ops[i].name.c_str(), i);
        bool sel = (state_.selected_target == i);
        ImGui::PushStyleColor(ImGuiCol_Button,
            sel ? ImVec4{0.957f, 0.722f, 0.271f, 0.32f} : ImVec4{0.957f, 0.722f, 0.271f, 0.12f});
        if (ImGui::Button(btn_id, ImVec2(100.0f, 28.0f))) {
            state_.selected_target = i;
            state_.dirty_ = true;
        }
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();

    // Draw the real |U_mesh| magnitude grid (each cell colored by magnitude,
    // labeled with |U_ij|). This is the propagated mesh unitary, not a mock.
    ImDrawList* dl    = ImGui::GetWindowDrawList();
    ImVec2 mat_origin = ImGui::GetCursorScreenPos();
    float cell_size   = 36.0f;
    int   mat_n       = engine::kN;

    for (int row = 0; row < mat_n; ++row) {
        for (int col = 0; col < mat_n; ++col) {
            std::complex<double> u = state_.eng.U_mesh.at(row, col);
            float mag = static_cast<float>(std::abs(u));
            float ph  = static_cast<float>(std::arg(u));   // -pi..pi
            float re_n = std::clamp(mag, 0.0f, 1.0f);
            float im_n = (ph + 3.14159f) / 6.28318f;        // phase -> 0..1

            float cx = mat_origin.x + col * cell_size;
            float cy = mat_origin.y + row * cell_size;

            // Magnitude on cyan, phase tint on purple.
            ImVec4 cell_col{
                kCyan.x * re_n + kPurple.x * im_n * 0.4f,
                kCyan.y * re_n * 0.6f + kPurple.y * im_n * 0.3f,
                kCyan.z * re_n * 0.5f + kPurple.z * im_n * 0.7f,
                0.9f
            };
            dl->AddRectFilled({cx+1, cy+1}, {cx+cell_size-1, cy+cell_size-1},
                ToU32(cell_col), 3.0f);

            char val_str[8];
            std::snprintf(val_str, sizeof(val_str), "%.2f", mag);
            dl->AddText({cx + 3.0f, cy + cell_size/2.0f - 6.0f},
                IM_COL32(220, 240, 255, 220), val_str);
        }
    }

    ImGui::Dummy(ImVec2(static_cast<float>(mat_n) * cell_size + 4.0f,
                         static_cast<float>(mat_n) * cell_size + 4.0f));

    ImGui::Spacing();
    float fid = state_.ops[state_.selected_target].fidelity;
    float err = state_.ops[state_.selected_target].error_db;
    float snr = state_.ops[state_.selected_target].snr_db;
    ImGui::TextColored(kMuted, "Fidelity |tr(U_target^H U_mesh)|/N: ");
    ImGui::SameLine();
    ImGui::TextColored(fid > 0.95f ? kSuccess : kGold, "%.2f%%", fid * 100.0f);
    ImGui::TextColored(kMuted, "Error (modeled): ");
    ImGui::SameLine();
    ImGui::TextColored(kGold, "%.2f dB", err);
    ImGui::SameLine(0.0f, 20.0f);
    ImGui::TextColored(kMuted, "SNR (modeled): ");
    ImGui::SameLine();
    ImGui::TextColored(snr > 20.0f ? kSuccess : kGold, "%.1f dB", snr);

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4{0.957f, 0.722f, 0.271f, 0.18f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.957f, 0.722f, 0.271f, 0.30f});
    // Real synthesis: shells out to the Solver decompose tool, applies phases,
    // recomputes. Blocks for the solver run (sub-second for N=4), then shows the
    // engine's measured fidelity. No fabricated progress bar.
    if (ImGui::Button("Decompose & Program", ImVec2(180.0f, 34.0f)))
        state_.program_target();
    ImGui::PopStyleColor(2);
    ImGui::SameLine();
    ImGui::TextColored(kMuted2, "synthesize selected target via Solver winch");

    if (!state_.synth_msg.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(state_.synth_ok ? kSuccess : kDanger, "%s", state_.synth_msg.c_str());
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void PhotonicsPanel::render_calibrate_tab() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::BeginChild("CalibratePanel", ImVec2(0.0f, 0.0f), true);

    ImGui::TextColored(kMuted, "CALIBRATION");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Insertion Loss (dB/MZI)");
    if (ImGui::SliderFloat("##il", &state_.mesh.insertion_loss_db, 0.0f, 3.0f, "%.2f dB"))
        state_.dirty_ = true;

    ImGui::Spacing();
    ImGui::Text("Crosstalk (dB)");
    if (ImGui::SliderFloat("##xtalk", &state_.mesh.crosstalk_db, -60.0f, -20.0f, "%.1f dB"))
        state_.dirty_ = true;

    ImGui::Spacing();
    ImGui::Text("Wavelength (nm)");
    if (ImGui::SliderFloat("##wl", &state_.mesh.wavelength_nm, 1500.0f, 1600.0f, "%.1f nm"))
        state_.dirty_ = true;

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4{0.133f, 0.773f, 0.447f, 0.18f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.133f, 0.773f, 0.447f, 0.30f});
    if (ImGui::Button("Run Calibration", ImVec2(160.0f, 34.0f))) { state_.dirty_ = true; }
    ImGui::PopStyleColor(2);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(kMuted, "Per-MZI Status (active Clements crossings)");
    ImGui::Spacing();

    if (ImGui::BeginTable("mzi_cal", 4,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY,
            ImVec2(0.0f, 200.0f))) {
        ImGui::TableSetupColumn("MZI",    ImGuiTableColumnFlags_WidthFixed, 50.0f);
        ImGui::TableSetupColumn("Trans.", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Heater", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (const auto& mzi : state_.mesh.mzis) {
            if (!mzi.active) continue;
            // |1 - transmission| is the bar-state error of this crossing.
            float err = std::fabs(1.0f - mzi.transmission);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("M%d", mzi.id);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(kCyan, "%.3f", mzi.transmission);
            ImGui::TableSetColumnIndex(2);
            ImGui::TextColored(kGold, "%.0f°", state_.heat_map[mzi.id]);
            ImGui::TableSetColumnIndex(3);
            ImVec4 sc = err < 0.5f ? kSuccess : kGold;
            ImGui::TextColored(sc, err < 0.5f ? "bar-dominant" : "cross-dominant");
        }
        ImGui::EndTable();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void PhotonicsPanel::render() {
    // Render-loop update (health-gui / flux-gui maybe_update/step pattern).
    state_.maybe_update();

    ImGui::TextColored(kCyan, "STRAYLIGHT");
    ImGui::SameLine();
    ImGui::TextColored(kGold, "Photonic Mesh Simulator (MZI unitary, no photonic HW)");
    ImGui::SameLine(0.0f, 20.0f);
    ImGui::TextColored(kMuted, "%s mesh  %dx%d  λ=%.0fnm",
        state_.mesh.topology.c_str(), state_.mesh.rows, state_.mesh.cols,
        state_.mesh.wavelength_nm);
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::BeginTabBar("photonics_tabs", ImGuiTabBarFlags_None)) {
        const char* tab_names[] = {"Mesh", "Detectors", "Matrix", "Calibrate"};
        for (int t = 0; t < 4; ++t) {
            if (ImGui::BeginTabItem(tab_names[t])) {
                state_.active_tab = t;
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    ImGui::Spacing();

    switch (state_.active_tab) {
        case 0: render_mesh_tab();       break;
        case 1: render_detectors_tab();  break;
        case 2: render_matrix_tab();     break;
        case 3: render_calibrate_tab();  break;
    }
}

} // namespace straylight::photonics
