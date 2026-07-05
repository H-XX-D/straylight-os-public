#pragma once
#include <imgui.h>
#include <string>
#include <vector>
#include <array>
#include <cmath>
#include "snn_engine.h"

namespace straylight::snn {

// Neuron, Synapse, SNNConfig and SimMetrics are defined in snn_engine.h so the
// standalone correctness harness can include the exact same math GUI-free.

struct SNNState {
    std::vector<Neuron>   neurons;
    std::vector<Synapse>  synapses;
    SNNConfig config;
    SimMetrics metrics;
    int active_tab = 0;
    bool simulating = false;
    float sim_progress = 0.0f;
    int   current_step = 0;
    std::vector<std::vector<int>> spike_times; // rolling raster: per-neuron spike steps

    // Live simulation engine + frame-pacing bookkeeping.
    SnnEngine engine;
    std::vector<int> input_ids;
    double last_step_time_ = -1.0e9;
    int    raster_step_    = 0;   // current column in the 100-step raster window
    long   spike_accum_    = 0;   // spikes accumulated in the current rate window
    double rate_window_ms_ = 0.0; // elapsed sim time in the current rate window

    void init();
    void step();        // advance the live simulation by config.dt_ms
    void maybe_step();  // call from render: paces step() to wall clock

private:
    void record_metrics(int spikes_this_step, float compute_ms);
};

class SNNPanel {
public:
    void init() { state_.init(); }
    void render();
private:
    SNNState state_;
    void render_network_tab();
    void render_activity_tab();
    void render_plasticity_tab();
    void render_config_tab();
};

} // namespace straylight::snn
