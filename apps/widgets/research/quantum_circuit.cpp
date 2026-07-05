// apps/widgets/research/quantum_circuit.cpp
#include "quantum_circuit.h"
#include "widget_registry.h"

#include <imgui.h>

REGISTER_WIDGET(straylight::widgets::QuantumCircuitWidget, "quantum_circuit", "Quantum Circuit", straylight::widgets::WidgetCategory::Research);
#include <cstdio>
#include <cmath>

namespace straylight::widgets {

const char* QuantumCircuitWidget::gate_label(GateType t) {
    switch (t) {
        case GateType::H:       return "H";
        case GateType::X:       return "X";
        case GateType::Y:       return "Y";
        case GateType::Z:       return "Z";
        case GateType::S:       return "S";
        case GateType::T:       return "T";
        case GateType::Rx:      return "Rx";
        case GateType::Ry:      return "Ry";
        case GateType::Rz:      return "Rz";
        case GateType::CNOT:    return "+";
        case GateType::CZ:      return "CZ";
        case GateType::SWAP:    return "x";
        case GateType::Toffoli: return "+";
        case GateType::Measure: return "M";
        case GateType::Barrier: return "|";
    }
    return "?";
}

ImU32 QuantumCircuitWidget::gate_color(GateType t) {
    switch (t) {
        case GateType::H:       return IM_COL32(100, 180, 255, 255);
        case GateType::X:       return IM_COL32(255, 100, 100, 255);
        case GateType::Y:       return IM_COL32(100, 255, 100, 255);
        case GateType::Z:       return IM_COL32(255, 255, 100, 255);
        case GateType::S:
        case GateType::T:       return IM_COL32(180, 100, 255, 255);
        case GateType::Rx:
        case GateType::Ry:
        case GateType::Rz:      return IM_COL32(255, 180, 80, 255);
        case GateType::CNOT:
        case GateType::Toffoli: return IM_COL32(80, 200, 255, 255);
        case GateType::CZ:      return IM_COL32(200, 200, 80, 255);
        case GateType::SWAP:    return IM_COL32(200, 100, 200, 255);
        case GateType::Measure: return IM_COL32(200, 200, 200, 255);
        case GateType::Barrier: return IM_COL32(150, 150, 150, 128);
    }
    return IM_COL32(255, 255, 255, 255);
}

void QuantumCircuitWidget::draw_gate(ImDrawList* dl, const QGate& gate, ImVec2 origin,
                                      float qubit_spacing, float layer_width) const {
    float cx = origin.x + layer_width * 0.5f;
    float cy = origin.y + gate.qubit * qubit_spacing;
    float box_size = 24.0f * zoom_;
    ImU32 col = gate_color(gate.type);

    if (gate.type == GateType::Barrier) {
        // Draw dashed vertical line across all qubits
        for (int q = 0; q < circuit_.num_qubits; ++q) {
            float y = origin.y + q * qubit_spacing;
            dl->AddLine(ImVec2(cx, y - box_size * 0.5f), ImVec2(cx, y + box_size * 0.5f),
                        col, 1.0f);
        }
        return;
    }

    if (gate.type == GateType::Measure) {
        // Draw meter symbol
        ImVec2 tl(cx - box_size * 0.5f, cy - box_size * 0.5f);
        ImVec2 br(cx + box_size * 0.5f, cy + box_size * 0.5f);
        dl->AddRectFilled(tl, br, IM_COL32(60, 60, 60, 255), 4.0f);
        dl->AddRect(tl, br, col, 4.0f);
        // Arc
        dl->AddBezierQuadratic(
            ImVec2(cx - 6 * zoom_, cy + 4 * zoom_),
            ImVec2(cx, cy - 8 * zoom_),
            ImVec2(cx + 6 * zoom_, cy + 4 * zoom_),
            col, 1.5f);
        // Arrow
        dl->AddLine(ImVec2(cx, cy), ImVec2(cx + 5 * zoom_, cy - 7 * zoom_), col, 1.5f);
        return;
    }

    // Draw control lines for multi-qubit gates
    if (gate.control >= 0) {
        float control_y = origin.y + gate.control * qubit_spacing;
        dl->AddLine(ImVec2(cx, control_y), ImVec2(cx, cy), col, 2.0f * zoom_);
        // Control dot
        dl->AddCircleFilled(ImVec2(cx, control_y), 4.0f * zoom_, col);
    }
    if (gate.control2 >= 0) {
        float control2_y = origin.y + gate.control2 * qubit_spacing;
        dl->AddLine(ImVec2(cx, control2_y), ImVec2(cx, cy), col, 2.0f * zoom_);
        dl->AddCircleFilled(ImVec2(cx, control2_y), 4.0f * zoom_, col);
    }

    if (gate.type == GateType::CNOT || gate.type == GateType::Toffoli) {
        // Target is circle with plus
        dl->AddCircle(ImVec2(cx, cy), 10.0f * zoom_, col, 0, 2.0f);
        dl->AddLine(ImVec2(cx - 7 * zoom_, cy), ImVec2(cx + 7 * zoom_, cy), col, 1.5f);
        dl->AddLine(ImVec2(cx, cy - 7 * zoom_), ImVec2(cx, cy + 7 * zoom_), col, 1.5f);
    } else if (gate.type == GateType::SWAP) {
        // X marks at both qubits
        float other_y = origin.y + gate.control * qubit_spacing;
        float s = 5.0f * zoom_;
        dl->AddLine(ImVec2(cx - s, cy - s), ImVec2(cx + s, cy + s), col, 2.0f);
        dl->AddLine(ImVec2(cx - s, cy + s), ImVec2(cx + s, cy - s), col, 2.0f);
        dl->AddLine(ImVec2(cx - s, other_y - s), ImVec2(cx + s, other_y + s), col, 2.0f);
        dl->AddLine(ImVec2(cx - s, other_y + s), ImVec2(cx + s, other_y - s), col, 2.0f);
    } else {
        // Standard gate box
        ImVec2 tl(cx - box_size * 0.5f, cy - box_size * 0.5f);
        ImVec2 br(cx + box_size * 0.5f, cy + box_size * 0.5f);
        dl->AddRectFilled(tl, br, IM_COL32(40, 40, 50, 230), 4.0f);
        dl->AddRect(tl, br, col, 4.0f, 0, 1.5f);

        const char* lbl = gate_label(gate.type);
        ImVec2 text_size = ImGui::CalcTextSize(lbl);
        dl->AddText(ImVec2(cx - text_size.x * 0.5f, cy - text_size.y * 0.5f), col, lbl);

        // Show angle for rotation gates
        if (gate.type == GateType::Rx || gate.type == GateType::Ry || gate.type == GateType::Rz) {
            char angle_buf[16];
            std::snprintf(angle_buf, sizeof(angle_buf), "%.2f", gate.angle);
            ImVec2 as = ImGui::CalcTextSize(angle_buf);
            dl->AddText(ImVec2(cx - as.x * 0.5f, cy + box_size * 0.5f + 1), IM_COL32(200, 200, 200, 180), angle_buf);
        }
    }
}

void QuantumCircuitWidget::load_demo_circuit() {
    circuit_.name = "Bell State + QFT Demo";
    circuit_.num_qubits = 4;
    circuit_.layers.clear();

    // Layer 0: H on q0
    circuit_.layers.push_back({{GateType::H, 0}});
    // Layer 1: CNOT q0->q1
    circuit_.layers.push_back({{GateType::CNOT, 1, 0}});
    // Layer 2: H on q2, T on q3
    circuit_.layers.push_back({{GateType::H, 2}, {GateType::T, 3}});
    // Layer 3: CNOT q2->q3
    circuit_.layers.push_back({{GateType::CNOT, 3, 2}});
    // Layer 4: Rz on q1
    QGate rz; rz.type = GateType::Rz; rz.qubit = 1; rz.angle = 1.5708f;
    circuit_.layers.push_back({rz});
    // Layer 5: Toffoli q0,q1->q2
    QGate toff; toff.type = GateType::Toffoli; toff.qubit = 2; toff.control = 0; toff.control2 = 1;
    circuit_.layers.push_back({toff});
    // Layer 6: SWAP q1,q3
    QGate sw; sw.type = GateType::SWAP; sw.qubit = 3; sw.control = 1;
    circuit_.layers.push_back({sw});
    // Layer 7: Measure all
    circuit_.layers.push_back({
        {GateType::Measure, 0}, {GateType::Measure, 1},
        {GateType::Measure, 2}, {GateType::Measure, 3}
    });

    has_circuit_ = true;
}

void QuantumCircuitWidget::load_circuit(const QuantumCircuit& circuit) {
    circuit_ = circuit;
    has_circuit_ = true;
    scroll_x_ = 0.0f;
}

void QuantumCircuitWidget::update() {
    if (!has_circuit_) load_demo_circuit();
}

void QuantumCircuitWidget::render(bool* p_open) {
    if (!ImGui::Begin("Quantum Circuit", p_open)) {
        ImGui::End();
        return;
    }

    if (!has_circuit_) {
        ImGui::TextWrapped("No circuit loaded.");
        ImGui::End();
        return;
    }

    // Controls
    ImGui::Text("Circuit: %s | Qubits: %d | Depth: %zu",
                circuit_.name.c_str(), circuit_.num_qubits, circuit_.layers.size());
    ImGui::SliderFloat("Zoom", &zoom_, 0.5f, 3.0f, "%.1fx");

    ImGui::Separator();

    // Drawing area
    ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
    ImVec2 canvas_size = ImGui::GetContentRegionAvail();
    if (canvas_size.y < 100) canvas_size.y = 100;

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Background
    dl->AddRectFilled(canvas_pos,
                      ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y),
                      IM_COL32(20, 20, 30, 255));

    float qubit_spacing = 40.0f * zoom_;
    float layer_width = 50.0f * zoom_;
    float margin_left = 50.0f;
    float margin_top = 20.0f;

    // Draw qubit wires and labels
    for (int q = 0; q < circuit_.num_qubits; ++q) {
        float y = canvas_pos.y + margin_top + q * qubit_spacing;
        float x_start = canvas_pos.x + margin_left;
        float x_end = canvas_pos.x + margin_left + static_cast<float>(circuit_.layers.size()) * layer_width + layer_width;

        // Wire
        dl->AddLine(ImVec2(x_start - scroll_x_, y),
                     ImVec2(x_end - scroll_x_, y),
                     IM_COL32(80, 80, 100, 200), 1.0f);

        // Label
        char qlabel[16];
        std::snprintf(qlabel, sizeof(qlabel), "q%d", q);
        dl->AddText(ImVec2(canvas_pos.x + 5, y - 7),
                     IM_COL32(180, 180, 200, 255), qlabel);
    }

    // Draw gates layer by layer
    for (size_t li = 0; li < circuit_.layers.size(); ++li) {
        float layer_x = canvas_pos.x + margin_left + static_cast<float>(li) * layer_width - scroll_x_;
        ImVec2 layer_origin(layer_x, canvas_pos.y + margin_top);

        for (auto& gate : circuit_.layers[li]) {
            draw_gate(dl, gate, layer_origin, qubit_spacing, layer_width);
        }
    }

    // Invisible widget for scrolling
    ImGui::SetCursorScreenPos(canvas_pos);
    ImGui::InvisibleButton("##canvas", canvas_size);
    if (ImGui::IsItemHovered()) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (ImGui::GetIO().KeyShift) {
            zoom_ += wheel * 0.1f;
            zoom_ = std::max(0.3f, std::min(4.0f, zoom_));
        } else {
            scroll_x_ -= wheel * 30.0f;
            float max_scroll = std::max(0.0f,
                static_cast<float>(circuit_.layers.size()) * layer_width - canvas_size.x + margin_left * 2);
            scroll_x_ = std::max(0.0f, std::min(scroll_x_, max_scroll));
        }
    }

    ImGui::End();
}

} // namespace straylight::widgets
