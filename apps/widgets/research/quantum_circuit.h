// apps/widgets/research/quantum_circuit.h
#pragma once

#include <straylight/widget.h>
#include <imgui.h>
#include <string>
#include <vector>

namespace straylight::widgets {

enum class GateType : uint8_t {
    H, X, Y, Z, S, T, Rx, Ry, Rz, CNOT, CZ, SWAP, Toffoli, Measure, Barrier
};

struct QGate {
    GateType type = GateType::H;
    int qubit = 0;            // target qubit
    int control = -1;         // control qubit (-1 if none)
    int control2 = -1;        // second control (Toffoli)
    float angle = 0.0f;       // rotation angle for Rx/Ry/Rz
};

struct QuantumCircuit {
    std::string name;
    int num_qubits = 0;
    std::vector<std::vector<QGate>> layers; // Each layer is a time step
};

class QuantumCircuitWidget : public WidgetBase {
public:
    const char* name() const override { return "Quantum Circuit"; }
    float poll_interval() const override { return 0.0f; } // No polling, user-driven
    void update() override;
    void render(bool* p_open) override;

    void load_circuit(const QuantumCircuit& circuit);

private:
    QuantumCircuit circuit_;
    float zoom_ = 1.0f;
    float scroll_x_ = 0.0f;
    bool has_circuit_ = false;

    // Demo circuit for when no circuit is loaded
    void load_demo_circuit();

    static const char* gate_label(GateType t);
    static ImU32 gate_color(GateType t);
    void draw_gate(ImDrawList* dl, const QGate& gate, ImVec2 origin,
                   float qubit_spacing, float layer_width) const;
};

} // namespace straylight::widgets
