// apps/widgets/research/snn_visualizer.cpp
#include "snn_visualizer.h"
#include "widget_registry.h"

#include <imgui.h>

REGISTER_WIDGET(straylight::widgets::SnnVisualizerWidget, "snn_visualizer", "SNN Visualizer", straylight::widgets::WidgetCategory::Research);
#include <algorithm>
#include <cstdio>
#include <cmath>

namespace straylight::widgets {

void SnnVisualizerWidget::try_connect() {
    auto res = ipc_.connect("/run/straylight/bus.sock");
    connected_ = res.has_value();
    if (!connected_) error_msg_ = res.error();
}

void SnnVisualizerWidget::fetch_state() {
    if (!connected_) return;

    auto res = ipc_.command("snn.state");
    if (!res.has_value()) {
        error_msg_ = res.error();
        connected_ = false;
        return;
    }

    auto& j = res.value();
    sim_time_ms_ = j.value("sim_time_ms", 0.0f);
    num_layers_ = j.value("num_layers", 0);
    total_spike_rate_ = j.value("spike_rate_hz", 0.0f);

    // Update neurons
    if (j.contains("neurons") && j["neurons"].is_array()) {
        for (auto& nj : j["neurons"]) {
            int id = nj.value("id", -1);
            if (id < 0) continue;

            SnnNeuron* found = nullptr;
            for (auto& n : neurons_) {
                if (n.id == id) { found = &n; break; }
            }
            if (!found) {
                neurons_.emplace_back();
                found = &neurons_.back();
                found->id = id;
            }
            found->layer = nj.value("layer", 0);
            found->membrane_v = nj.value("membrane_v", -70.0f);
            found->threshold = nj.value("threshold", -55.0f);
            found->spiked = nj.value("spiked", false);
            push_voltage_trace(*found);
        }
    }

    // Recent spikes
    if (j.contains("spikes") && j["spikes"].is_array()) {
        recent_spikes_.clear();
        for (auto& sj : j["spikes"]) {
            SpikeEvent se;
            se.neuron_id = sj.value("neuron_id", 0);
            se.layer = sj.value("layer", 0);
            se.time_ms = sj.value("time_ms", 0.0f);
            recent_spikes_.push_back(se);
        }
    }
}

void SnnVisualizerWidget::push_voltage_trace(SnnNeuron& n) {
    int idx = n.trace_offset % SnnNeuron::kTraceLen;
    n.voltage_trace[idx] = n.membrane_v;
    n.trace_offset++;
}

void SnnVisualizerWidget::update() {
    if (!should_update()) return;
    if (!connected_) try_connect();
    if (connected_) fetch_state();
}

void SnnVisualizerWidget::render(bool* p_open) {
    if (!ImGui::Begin("SNN Visualizer", p_open)) {
        ImGui::End();
        return;
    }

    if (!connected_) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Disconnected from straylight-bus");
        if (!error_msg_.empty()) ImGui::TextWrapped("Error: %s", error_msg_.c_str());
        if (ImGui::Button("Retry")) try_connect();
        ImGui::End();
        return;
    }

    // Header
    ImGui::Text("Sim Time: %.1f ms | Layers: %d | Neurons: %zu | Spike Rate: %.1f Hz",
                sim_time_ms_, num_layers_, neurons_.size(), total_spike_rate_);

    ImGui::RadioButton("Raster", &view_mode_, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Voltage", &view_mode_, 1);

    ImGui::Separator();

    if (view_mode_ == 0) {
        // Spike Raster Plot
        // X axis = time, Y axis = neuron ID
        ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
        ImVec2 canvas_size = ImGui::GetContentRegionAvail();
        if (canvas_size.y < 100) canvas_size.y = 100;

        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(canvas_pos,
                          ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y),
                          IM_COL32(15, 15, 25, 255));

        if (!recent_spikes_.empty() && !neurons_.empty()) {
            // Find time range
            float t_min = recent_spikes_.front().time_ms;
            float t_max = recent_spikes_.back().time_ms;
            if (t_max <= t_min) t_max = t_min + 1.0f;

            int max_neuron = 0;
            for (auto& n : neurons_) max_neuron = std::max(max_neuron, n.id);
            if (max_neuron == 0) max_neuron = 1;

            float margin = 30.0f;
            float plot_w = canvas_size.x - margin * 2;
            float plot_h = canvas_size.y - margin * 2;

            for (auto& se : recent_spikes_) {
                float x = canvas_pos.x + margin + ((se.time_ms - t_min) / (t_max - t_min)) * plot_w;
                float y = canvas_pos.y + margin + (static_cast<float>(se.neuron_id) / static_cast<float>(max_neuron)) * plot_h;

                // Color by layer
                ImU32 col;
                int lc = se.layer % 6;
                switch (lc) {
                    case 0: col = IM_COL32(255, 100, 100, 255); break;
                    case 1: col = IM_COL32(100, 255, 100, 255); break;
                    case 2: col = IM_COL32(100, 100, 255, 255); break;
                    case 3: col = IM_COL32(255, 255, 100, 255); break;
                    case 4: col = IM_COL32(255, 100, 255, 255); break;
                    default: col = IM_COL32(100, 255, 255, 255); break;
                }
                dl->AddLine(ImVec2(x, y - 1), ImVec2(x, y + 1), col, 2.0f);
            }

            // Axis labels
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.0f ms", t_min);
            dl->AddText(ImVec2(canvas_pos.x + margin, canvas_pos.y + canvas_size.y - 15),
                        IM_COL32(180, 180, 180, 200), buf);
            std::snprintf(buf, sizeof(buf), "%.0f ms", t_max);
            dl->AddText(ImVec2(canvas_pos.x + canvas_size.x - 60, canvas_pos.y + canvas_size.y - 15),
                        IM_COL32(180, 180, 180, 200), buf);
        }

        ImGui::SetCursorScreenPos(ImVec2(canvas_pos.x, canvas_pos.y + canvas_size.y));
        ImGui::Dummy(ImVec2(0, 0));

    } else {
        // Voltage trace view
        // Show selectable neurons
        ImGui::BeginChild("##neuron_list", ImVec2(120, 0), true);
        for (int i = 0; i < static_cast<int>(neurons_.size()); ++i) {
            auto& n = neurons_[i];
            char lbl[32]; std::snprintf(lbl, sizeof(lbl), "N%d (L%d)", n.id, n.layer);
            if (ImGui::Selectable(lbl, selected_neuron_ == i)) {
                selected_neuron_ = i;
            }
        }
        ImGui::EndChild();

        ImGui::SameLine();

        // Voltage plot
        ImGui::BeginChild("##voltage_plot", ImVec2(0, 0), true);
        if (selected_neuron_ >= 0 && selected_neuron_ < static_cast<int>(neurons_.size())) {
            auto& n = neurons_[selected_neuron_];
            ImGui::Text("Neuron %d | Layer %d | V=%.1f mV | Thresh=%.1f mV",
                        n.id, n.layer, n.membrane_v, n.threshold);

            int count = std::min(n.trace_offset, SnnNeuron::kTraceLen);
            if (count > 0) {
                std::array<float, SnnNeuron::kTraceLen> plot{};
                for (int j = 0; j < count; ++j) {
                    int src = (n.trace_offset - count + j) % SnnNeuron::kTraceLen;
                    plot[j] = n.voltage_trace[src];
                }

                // Draw threshold line reference
                ImGui::Text("Membrane Potential");
                ImGui::PlotLines("##v_trace", plot.data(), count,
                                 0, nullptr, -80.0f, 40.0f,
                                 ImVec2(-1, ImGui::GetContentRegionAvail().y - 20));
                ImGui::Text("Threshold: %.1f mV (dashed line not drawn in PlotLines)", n.threshold);
            }
        } else {
            ImGui::TextWrapped("Select a neuron from the list to view its voltage trace.");
        }
        ImGui::EndChild();
    }

    ImGui::End();
}

} // namespace straylight::widgets
