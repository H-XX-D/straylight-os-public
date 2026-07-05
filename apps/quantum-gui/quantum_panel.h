#pragma once
#include <imgui.h>
#include <string>
#include <vector>
#include <complex>
#include <array>

namespace straylight::quantum {

struct Gate {
    std::string name;
    int qubit;
    int target_qubit;
    float angle;
};

struct CircuitState {
    int num_qubits = 4;
    std::vector<std::vector<Gate>> layers;
    std::vector<Gate> flat_circuit;
    std::string circuit_name = "Bell State";
    bool show_matrix = false;
};

struct SimResult {
    std::vector<float> probabilities;
    std::vector<std::string> basis_labels;
    float fidelity = 0.0f;
    int   shots = 1024;
    std::vector<std::pair<std::string, int>> measurement_counts;
    float circuit_depth = 0.0f;
    float t1_time_us = 0.0f;
    float t2_time_us = 0.0f;
};

struct NoiseModel {
    bool  enabled = false;
    float depolarizing = 0.001f;
    float readout_error = 0.005f;
    float t1_us = 100.0f;
    float t2_us = 80.0f;
};

struct QuantumState {
    CircuitState circuit;
    SimResult    result;
    NoiseModel   noise;
    int active_tab = 0;
    bool simulating = false;
    float sim_progress = 0.0f;
    int bloch_qubit = 0;
    float bloch_theta = 0.0f;
    float bloch_phi   = 0.0f;

    void init();
    // Real statevector simulation: builds amp = |0>, applies circuit.flat_circuit
    // in order, and fills every SimResult field the panel renders.
    void simulate();
};

class QuantumPanel {
public:
    void init() { state_.init(); }
    void render();
private:
    QuantumState state_;
    void render_circuit_tab();
    void render_results_tab();
    void render_noise_tab();
    void render_library_tab();
    void render_bloch_sphere();
};

} // namespace straylight::quantum
