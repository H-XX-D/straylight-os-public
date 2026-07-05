// apps/widgets/research/snn_visualizer.h
#pragma once

#include <straylight/widget.h>
#include <straylight/ipc_client.h>
#include <array>
#include <string>
#include <vector>

namespace straylight::widgets {

struct SnnNeuron {
    int id = 0;
    int layer = 0;
    float membrane_v = -70.0f; // mV
    float threshold = -55.0f;
    bool spiked = false;
    // Voltage trace (ring buffer)
    static constexpr int kTraceLen = 200;
    std::array<float, kTraceLen> voltage_trace{};
    int trace_offset = 0;
};

struct SpikeEvent {
    int neuron_id = 0;
    int layer = 0;
    float time_ms = 0.0f;
};

class SnnVisualizerWidget : public WidgetBase {
public:
    const char* name() const override { return "SNN Visualizer"; }
    float poll_interval() const override { return 0.1f; }
    void update() override;
    void render(bool* p_open) override;

private:
    IpcJsonClient ipc_;
    bool connected_ = false;
    std::vector<SnnNeuron> neurons_;
    std::vector<SpikeEvent> recent_spikes_;
    float sim_time_ms_ = 0.0f;
    int num_layers_ = 0;
    float total_spike_rate_ = 0.0f;
    std::string error_msg_;
    int selected_neuron_ = -1;
    int view_mode_ = 0; // 0=raster, 1=voltage

    void try_connect();
    void fetch_state();
    void push_voltage_trace(SnnNeuron& n);
};

} // namespace straylight::widgets
