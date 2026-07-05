// apps/quantum-gui/quantum_panel.cpp
#include "quantum_panel.h"
#include "quantum_engine.h"
#include <imgui.h>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <complex>
#include <cstdint>

namespace straylight::quantum {

static constexpr ImVec4 kCyan     = {0.098f, 0.906f, 1.000f, 1.0f};
static constexpr ImVec4 kPurple   = {0.545f, 0.361f, 0.965f, 1.0f};
static constexpr ImVec4 kGold     = {0.957f, 0.722f, 0.271f, 1.0f};
static constexpr ImVec4 kBgPanel  = {0.035f, 0.055f, 0.110f, 0.90f};
static constexpr ImVec4 kBorder   = {1.0f, 1.0f, 1.0f, 0.08f};
static constexpr ImVec4 kMuted    = {1.0f, 1.0f, 1.0f, 0.55f};
static constexpr ImVec4 kMuted2   = {1.0f, 1.0f, 1.0f, 0.30f};
static constexpr ImVec4 kSuccess  = {0.133f, 0.773f, 0.447f, 1.0f};
static constexpr ImVec4 kDanger   = {1.0f, 0.298f, 0.416f, 1.0f};

static ImU32 ToU32(ImVec4 v) {
    return IM_COL32(
        static_cast<int>(v.x * 255),
        static_cast<int>(v.y * 255),
        static_cast<int>(v.z * 255),
        static_cast<int>(v.w * 255));
}

void QuantumState::init() {
    circuit.num_qubits = 4;
    circuit.circuit_name = "Bell State";

    // Circuit *input* (not a fabricated output): H on q0, CNOT q0->q1, mirrored
    // on q2/q3. The result fields below are computed from this by simulate().
    circuit.flat_circuit = {
        {"H",    0, -1, 0.0f},
        {"CNOT", 0,  1, 0.0f},
        {"H",    2, -1, 0.0f},
        {"CNOT", 2,  3, 0.0f},
    };

    simulate();
}

void QuantumState::simulate() {
    namespace eng = straylight::quantum::engine;

    const int n = circuit.num_qubits;

    // Translate the GUI gate list into engine gates and evolve |0...0>.
    std::vector<eng::EngineGate> gates;
    gates.reserve(circuit.flat_circuit.size());
    for (const auto& g : circuit.flat_circuit) {
        if (g.qubit < 0 || g.qubit >= n) continue;            // skip out-of-range
        if (g.target_qubit >= n) continue;
        gates.push_back({g.name, g.qubit, g.target_qubit, static_cast<double>(g.angle)});
    }

    eng::Statevector sv(n);
    sv.run(gates);
    const auto& amp = sv.amplitudes();

    // probabilities[i] = |amp[i]|^2 ; basis_labels[i] = n-bit binary.
    const std::size_t dim = amp.size();
    result.probabilities.assign(dim, 0.0f);
    result.basis_labels.assign(dim, std::string());
    std::vector<double> probs_d(dim, 0.0);
    for (std::size_t i = 0; i < dim; ++i) {
        probs_d[i] = std::norm(amp[i]);
        result.probabilities[i] = static_cast<float>(probs_d[i]);
        result.basis_labels[i]  = eng::basis_label(i, n);
    }

    // measurement_counts: sample `shots` draws from the distribution.
    result.measurement_counts = eng::sample_counts(probs_d, n, result.shots, 0x5715u);

    // circuit_depth via greedy per-qubit layering.
    result.circuit_depth = static_cast<float>(eng::circuit_depth(gates, n));

    // fidelity from the depolarizing noise model (1-p when enabled, else ideal).
    result.fidelity = static_cast<float>(
        eng::noise_fidelity(noise.enabled, noise.depolarizing));

    // T1/T2 reported straight from the noise model (the panel only displays them).
    result.t1_time_us = noise.t1_us;
    result.t2_time_us = noise.t2_us;

    // Bloch vector for the selected qubit from its reduced single-qubit state.
    // Reduce amp to the 2x2 density matrix of qubit `bloch_qubit`, then read the
    // Bloch angles. theta = arccos(z), phi = atan2(<Y>, <X>).
    int bq = bloch_qubit;
    if (bq < 0 || bq >= n) bq = 0;
    const std::size_t bbit = std::size_t(1) << bq;
    std::complex<double> rho01(0.0, 0.0); // <0|rho|1> for the qubit
    double p1 = 0.0;                       // probability qubit = 1
    for (std::size_t i = 0; i < dim; ++i) {
        if (i & bbit) { p1 += std::norm(amp[i]); }
        else {
            // coherence term: amp[i] (qubit=0) * conj(amp[i|bbit]) (qubit=1)
            rho01 += amp[i] * std::conj(amp[i | bbit]);
        }
    }
    const double x =  2.0 * rho01.real();
    const double y = -2.0 * rho01.imag();
    const double z =  1.0 - 2.0 * p1;
    double zc = z; if (zc > 1.0) zc = 1.0; if (zc < -1.0) zc = -1.0;
    bloch_theta = static_cast<float>(std::acos(zc));
    bloch_phi   = static_cast<float>(std::atan2(y, x));
}

void QuantumPanel::render_bloch_sphere() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 cursor = ImGui::GetCursorScreenPos();
    float cx = cursor.x + 60.0f;
    float cy = cursor.y + 60.0f;
    float r  = 54.0f;

    // Draw sphere outline
    dl->AddCircle({cx, cy}, r, IM_COL32(80,120,160,120), 64, 1.5f);

    // Axes
    dl->AddLine({cx - r, cy}, {cx + r, cy}, IM_COL32(60,60,80,200), 1.0f);
    dl->AddLine({cx, cy - r}, {cx, cy + r}, IM_COL32(60,60,80,200), 1.0f);

    // State vector point on Bloch sphere
    float bx = cx + r * std::sin(state_.bloch_theta) * std::cos(state_.bloch_phi);
    float by = cy - r * std::cos(state_.bloch_theta);
    dl->AddLine({cx, cy}, {bx, by}, ToU32(kCyan), 2.0f);
    dl->AddCircleFilled({bx, by}, 6.0f, ToU32(kCyan));

    // Labels
    dl->AddText({cx + r + 4, cy - 7}, IM_COL32(180,180,180,200), "X");
    dl->AddText({cx - 8, cy - r - 14}, IM_COL32(180,180,180,200), "Z");

    ImGui::Dummy(ImVec2(120.0f, 120.0f));
}

void QuantumPanel::render_circuit_tab() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::BeginChild("CircuitPanel", ImVec2(0.0f, 0.0f), true);

    ImGui::TextColored(kCyan, "QUANTUM CIRCUIT");
    ImGui::SameLine(0.0f, 16.0f);
    ImGui::TextColored(kMuted, "%s", state_.circuit.circuit_name.c_str());
    ImGui::Separator();
    ImGui::Spacing();

    // Gate palette
    const char* gate_names[] = {"H", "X", "Y", "Z", "T", "S", "RZ", "CNOT"};
    ImGui::TextColored(kMuted, "Gates:");
    ImGui::SameLine();
    for (int i = 0; i < 8; ++i) {
        if (i > 0) ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4{0.098f, 0.906f, 1.000f, 0.10f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.098f, 0.906f, 1.000f, 0.22f});
        char btn_id[16];
        std::snprintf(btn_id, sizeof(btn_id), "%s##gp%d", gate_names[i], i);
        ImGui::Button(btn_id, ImVec2(40.0f, 28.0f));
        ImGui::PopStyleColor(2);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(kMuted, "Circuit (%d qubits)", state_.circuit.num_qubits);
    ImGui::Spacing();

    // Visual circuit using DrawList
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float qubit_spacing = 52.0f;
    float gate_size     = 38.0f;
    float col_spacing   = 56.0f;
    float qubit_y_start = origin.y + 16.0f;
    float wire_start    = origin.x + 20.0f;

    // Count max columns
    int max_col = 4;
    float wire_end = wire_start + (max_col + 1) * col_spacing + gate_size + 20.0f;

    // Draw qubit wires
    for (int q = 0; q < state_.circuit.num_qubits; ++q) {
        float wy = qubit_y_start + q * qubit_spacing + gate_size / 2.0f;
        dl->AddLine({wire_start, wy}, {wire_end, wy}, IM_COL32(60,90,120,180), 1.5f);
        char qlabel[32];
        std::snprintf(qlabel, sizeof(qlabel), "q%d", q);
        dl->AddText({origin.x, wy - 7.0f}, IM_COL32(120,180,200,220), qlabel);
    }

    // Draw gates
    for (int gi = 0; gi < static_cast<int>(state_.circuit.flat_circuit.size()); ++gi) {
        const auto& g = state_.circuit.flat_circuit[gi];
        float gx = wire_start + (gi + 1) * col_spacing;
        float gy = qubit_y_start + g.qubit * qubit_spacing;

        if (g.name == "CNOT" && g.target_qubit >= 0) {
            // Control dot
            float ctrl_cy = gy + gate_size / 2.0f;
            float tgt_cy  = qubit_y_start + g.target_qubit * qubit_spacing + gate_size / 2.0f;
            float cx2     = gx + gate_size / 2.0f;
            dl->AddLine({cx2, ctrl_cy}, {cx2, tgt_cy}, ToU32(kCyan), 2.0f);
            dl->AddCircleFilled({cx2, ctrl_cy}, 6.0f, ToU32(kCyan));
            // Target ⊕
            float tr = 10.0f;
            dl->AddCircle({cx2, tgt_cy}, tr, ToU32(kCyan), 16, 2.0f);
            dl->AddLine({cx2 - tr, tgt_cy}, {cx2 + tr, tgt_cy}, ToU32(kCyan), 2.0f);
            dl->AddLine({cx2, tgt_cy - tr}, {cx2, tgt_cy + tr}, ToU32(kCyan), 2.0f);
        } else {
            // Single qubit gate box
            dl->AddRectFilled({gx, gy}, {gx + gate_size, gy + gate_size},
                IM_COL32(10,80,100,200), 6.0f);
            dl->AddRect({gx, gy}, {gx + gate_size, gy + gate_size},
                ToU32(kCyan), 6.0f, 0, 1.5f);
            float text_x = gx + gate_size / 2.0f - static_cast<float>(g.name.size()) * 3.5f;
            dl->AddText({text_x, gy + gate_size / 2.0f - 7.0f},
                IM_COL32(25,230,255,255), g.name.c_str());
        }
    }

    // Reserve space for circuit drawing
    ImGui::Dummy(ImVec2(wire_end - origin.x, state_.circuit.num_qubits * qubit_spacing + 20.0f));

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Controls
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.098f, 0.906f, 1.000f, 0.12f});
    if (ImGui::Button("Add Qubit", ImVec2(110.0f, 32.0f))) {
        if (state_.circuit.num_qubits < 8) state_.circuit.num_qubits++;
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear", ImVec2(80.0f, 32.0f))) {
        state_.circuit.flat_circuit.clear();
    }
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4{0.098f, 0.906f, 1.000f, 0.20f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.098f, 0.906f, 1.000f, 0.35f});
    if (ImGui::Button("Run Simulation", ImVec2(150.0f, 32.0f))) {
        // Run the real statevector engine over the current circuit + noise model.
        state_.simulate();
        state_.simulating = true;       // drive the progress-bar animation only
        state_.sim_progress = 0.0f;
    }
    ImGui::PopStyleColor(2);

    if (state_.simulating) {
        ImGui::Spacing();
        char prog_label[32];
        std::snprintf(prog_label, sizeof(prog_label), "%.0f%%", state_.sim_progress * 100.0f);
        ImGui::ProgressBar(state_.sim_progress, ImVec2(-1.0f, 0.0f), prog_label);
    }

    ImGui::Spacing();
    ImGui::TextColored(kMuted, "Bloch Sphere (q%d)", state_.bloch_qubit);
    render_bloch_sphere();

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void QuantumPanel::render_results_tab() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::BeginChild("ResultsPanel", ImVec2(0.0f, 0.0f), true);

    ImGui::TextColored(kPurple, "SIMULATION RESULTS");
    ImGui::Separator();
    ImGui::Spacing();

    // Metrics row
    ImGui::TextColored(kMuted, "Fidelity:");
    ImGui::SameLine();
    ImGui::TextColored(kSuccess, "%.4f", state_.result.fidelity);
    ImGui::SameLine(0.0f, 30.0f);
    ImGui::TextColored(kMuted, "Shots:");
    ImGui::SameLine();
    ImGui::TextColored(kCyan, "%d", state_.result.shots);
    ImGui::SameLine(0.0f, 30.0f);
    ImGui::TextColored(kMuted, "Depth:");
    ImGui::SameLine();
    ImGui::TextColored(kGold, "%.0f", state_.result.circuit_depth);

    ImGui::Spacing();
    ImGui::TextColored(kMuted, "T1: %.1f µs", state_.result.t1_time_us);
    ImGui::SameLine(0.0f, 20.0f);
    ImGui::TextColored(kMuted, "T2: %.1f µs", state_.result.t2_time_us);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(kMuted, "Probability Distribution (|ψ|²)");
    ImGui::Spacing();

    // Probability histogram using DrawList
    int n_states = static_cast<int>(state_.result.probabilities.size());
    if (n_states > 0) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 origin  = ImGui::GetCursorScreenPos();
        float avail_w  = ImGui::GetContentRegionAvail().x;
        float hist_h   = 100.0f;
        float bar_w    = avail_w / static_cast<float>(n_states) - 2.0f;

        for (int i = 0; i < n_states; ++i) {
            float prob = state_.result.probabilities[i];
            if (prob < 0.001f) continue;
            float bx = origin.x + i * (avail_w / static_cast<float>(n_states));
            float bh = prob * hist_h;
            float by = origin.y + hist_h - bh;
            // Gradient: purple base, cyan top
            dl->AddRectFilledMultiColor(
                {bx, by}, {bx + bar_w, origin.y + hist_h},
                ToU32(kCyan), ToU32(kCyan),
                ToU32(kPurple), ToU32(kPurple));
        }
        ImGui::Dummy(ImVec2(avail_w, hist_h + 4.0f));
    }

    // Fidelity gauge
    ImGui::Spacing();
    ImGui::Text("Fidelity gauge:");
    char fid_label[16];
    std::snprintf(fid_label, sizeof(fid_label), "%.2f%%", state_.result.fidelity * 100.0f);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kSuccess);
    ImGui::ProgressBar(state_.result.fidelity, ImVec2(-1.0f, 0.0f), fid_label);
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(kMuted, "Measurement Counts");
    ImGui::Spacing();

    if (ImGui::BeginTable("meas_counts", 2,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("State",  ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Count",  ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const auto& [st, cnt] : state_.result.measurement_counts) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(kCyan, "|%s⟩", st.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%d", cnt);
        }
        ImGui::EndTable();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void QuantumPanel::render_noise_tab() {
    auto& n = state_.noise;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::BeginChild("NoisePanel", ImVec2(0.0f, 0.0f), true);

    ImGui::TextColored(kGold, "NOISE MODEL");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Checkbox("Enable noise model", &n.enabled);
    ImGui::Spacing();

    ImGui::BeginDisabled(!n.enabled);

    ImGui::Text("Depolarizing Rate");
    ImGui::SliderFloat("##depol", &n.depolarizing, 0.0001f, 0.05f, "%.4f");

    ImGui::Spacing();
    ImGui::Text("Readout Error");
    ImGui::SliderFloat("##readout", &n.readout_error, 0.0001f, 0.1f, "%.4f");

    ImGui::Spacing();
    ImGui::Text("T1 Relaxation (µs)");
    ImGui::SliderFloat("##t1", &n.t1_us, 1.0f, 500.0f, "%.1f µs");

    ImGui::Spacing();
    ImGui::Text("T2 Dephasing (µs)");
    ImGui::SliderFloat("##t2", &n.t2_us, 1.0f, 500.0f, "%.1f µs");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(kMuted, "Presets:");
    ImGui::Spacing();

    if (ImGui::Button("Ideal", ImVec2(80.0f, 30.0f))) {
        n.depolarizing = 0.0f; n.readout_error = 0.0f;
        n.t1_us = 1000.0f; n.t2_us = 1000.0f;
    }
    ImGui::SameLine();
    if (ImGui::Button("NISQ-Low", ImVec2(100.0f, 30.0f))) {
        n.depolarizing = 0.001f; n.readout_error = 0.005f;
        n.t1_us = 100.0f; n.t2_us = 80.0f;
    }
    ImGui::SameLine();
    if (ImGui::Button("NISQ-High", ImVec2(100.0f, 30.0f))) {
        n.depolarizing = 0.01f; n.readout_error = 0.02f;
        n.t1_us = 30.0f; n.t2_us = 20.0f;
    }

    ImGui::EndDisabled();

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void QuantumPanel::render_library_tab() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::BeginChild("LibraryPanel", ImVec2(0.0f, 0.0f), true);

    ImGui::TextColored(kCyan, "CIRCUIT LIBRARY");
    ImGui::Separator();
    ImGui::Spacing();

    struct CircuitPreset { const char* name; const char* desc; int qubits; };
    static const CircuitPreset presets[] = {
        {"Bell State",  "Maximally entangled 2-qubit state", 2},
        {"GHZ",         "Greenberger-Horne-Zeilinger state", 3},
        {"QFT-4",       "4-qubit Quantum Fourier Transform",  4},
        {"Grover-2q",   "2-qubit Grover search oracle",       2},
        {"VQE-ansatz",  "Variational quantum eigensolver",    4},
    };

    float avail_w = ImGui::GetContentRegionAvail().x;
    float card_w  = (avail_w - 20.0f) / 3.0f;

    for (int i = 0; i < 5; ++i) {
        if (i % 3 != 0) ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0.040f, 0.065f, 0.130f, 1.0f});
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);

        char card_id[32];
        std::snprintf(card_id, sizeof(card_id), "preset_card_%d", i);
        ImGui::BeginChild(card_id, ImVec2(card_w, 90.0f), true);

        ImGui::TextColored(kCyan, "%s", presets[i].name);
        ImGui::TextColored(kMuted, "%s", presets[i].desc);
        ImGui::TextColored(kMuted2, "Qubits: %d", presets[i].qubits);
        ImGui::Spacing();

        if (ImGui::Button("Load##preset", ImVec2(-1.0f, 22.0f))) {
            state_.circuit.circuit_name = presets[i].name;
            state_.circuit.num_qubits   = presets[i].qubits;
        }

        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void QuantumPanel::render() {
    if (state_.simulating) {
        state_.sim_progress += 0.008f;
        if (state_.sim_progress >= 1.0f) { state_.simulating = false; state_.sim_progress = 1.0f; }
    }

    ImGui::TextColored(kCyan, "STRAYLIGHT");
    ImGui::SameLine();
    ImGui::TextColored(kPurple, "Quantum Simulator (statevector, no QPU on host)");
    ImGui::SameLine(0.0f, 20.0f);
    ImGui::TextColored(kMuted, "%d-qubit circuit", state_.circuit.num_qubits);
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::BeginTabBar("quantum_tabs", ImGuiTabBarFlags_None)) {
        const char* tab_names[] = {"Circuit", "Results", "Noise", "Library"};
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
        case 0: render_circuit_tab();  break;
        case 1: render_results_tab();  break;
        case 2: render_noise_tab();    break;
        case 3: render_library_tab();  break;
    }
}

} // namespace straylight::quantum
