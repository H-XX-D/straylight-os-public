// apps/snn-gui/snn_harness.cpp
// Standalone correctness harness for the LIF engine. No imgui, no GUI.
// Compile: g++ -std=c++20 snn_harness.cpp -o snn_harness
//
// Invariants checked over >=1000 steps under Poisson input drive:
//   1. every membrane_v is finite and within [-90, 0] mV at all times
//   2. spikes actually occur (final spike_rate_hz > 0)
//   3. immediately after a spike, V is reset strictly below threshold
#include "snn_engine.h"
#include <cassert>
#include <cstdio>
#include <cmath>

using namespace straylight::snn;

int main() {
    SNNConfig cfg;                 // defaults: 3 layers {8,16,8}, dt=1ms, 50 Hz in
    cfg.input_rate_hz = 80.0f;     // healthy drive
    cfg.stdp_enabled  = true;

    std::vector<Neuron>  neurons;
    std::vector<Synapse> synapses;
    std::vector<int>     input_ids;
    build_network(cfg, neurons, synapses, input_ids);

    SnnEngine eng;
    eng.reset(neurons.size(), synapses.size(), synapses, input_ids);

    const int   STEPS = 1500;
    const float dt    = cfg.dt_ms;

    long   total_spikes      = 0;
    bool   any_nonfinite     = false;
    bool   any_out_of_range  = false;
    bool   reset_violation   = false;
    float  min_v             =  1e9f, max_v = -1e9f;
    int    steps_with_spikes = 0;

    for (int s = 0; s < STEPS; ++s) {
        int spk = eng.step(neurons, synapses, cfg, dt);
        total_spikes += spk;
        if (spk > 0) ++steps_with_spikes;

        for (const auto& n : neurons) {
            float v = n.membrane_v;
            if (!std::isfinite(v)) any_nonfinite = true;
            if (v < kVFloor - 1e-3f || v > kVCeil + 1e-3f) any_out_of_range = true;
            if (v < min_v) min_v = v;
            if (v > max_v) max_v = v;
            // Invariant 3: a neuron flagged spiking this step must have been
            // reset strictly below its threshold.
            if (n.spiking && !(v < n.threshold)) reset_violation = true;
        }
    }

    double sim_seconds   = (STEPS * dt) / 1000.0;
    double spike_rate_hz = total_spikes / sim_seconds;

    std::printf("=== SNN LIF engine correctness harness ===\n");
    std::printf("network        : %zu neurons, %zu synapses, %zu input neurons\n",
                neurons.size(), synapses.size(), input_ids.size());
    std::printf("steps          : %d  (dt=%.2f ms, %.3f s sim time)\n",
                STEPS, dt, sim_seconds);
    std::printf("total spikes   : %ld\n", total_spikes);
    std::printf("steps w/ spikes: %d / %d\n", steps_with_spikes, STEPS);
    std::printf("spike_rate_hz  : %.2f Hz (network aggregate)\n", spike_rate_hz);
    std::printf("membrane V min : %.3f mV\n", min_v);
    std::printf("membrane V max : %.3f mV\n", max_v);
    std::printf("threshold      : %.2f mV\n", kVThreshold);
    std::printf("\n-- invariant checks --\n");
    std::printf("all V finite              : %s\n", any_nonfinite    ? "FAIL" : "PASS");
    std::printf("all V in [-90,0] mV       : %s\n", any_out_of_range ? "FAIL" : "PASS");
    std::printf("spikes occurred (>0 Hz)   : %s\n", spike_rate_hz > 0 ? "PASS" : "FAIL");
    std::printf("V reset < threshold @spike: %s\n", reset_violation  ? "FAIL" : "PASS");

    bool ok = !any_nonfinite && !any_out_of_range && spike_rate_hz > 0.0 && !reset_violation;
    std::printf("\nRESULT: %s\n", ok ? "ALL INVARIANTS HOLD" : "INVARIANT VIOLATED");

    assert(!any_nonfinite     && "membrane potential became non-finite");
    assert(!any_out_of_range  && "membrane potential left [-90,0] mV");
    assert(spike_rate_hz > 0.0 && "no spikes occurred under input drive");
    assert(!reset_violation   && "V not reset below threshold after spike");

    return ok ? 0 : 1;
}
