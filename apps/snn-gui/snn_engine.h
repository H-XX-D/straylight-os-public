// apps/snn-gui/snn_engine.h
// StrayLight SNN — real leaky integrate-and-fire (LIF) simulation engine.
//
// This header is imgui-free on purpose: the GUI panel includes it, and the
// standalone correctness harness includes the exact same math with no GUI deps.
//
// Model (per neuron, per step of dt milliseconds):
//   dV = (V_rest - V + R*I_syn + I_input) / tau_m * dt
// When V crosses threshold the neuron emits a spike, V is reset to V_reset,
// and each outgoing synapse injects weight-scaled current into its post neuron
// for the next step. Poisson input drive feeds the first layer at the configured
// input rate. Optional STDP writes Synapse::stdp_delta from pre/post spike timing.
#pragma once

#include <vector>
#include <array>
#include <string>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <algorithm>

namespace straylight::snn {

// ---- Biophysical constants (millivolt scale) -------------------------------
inline constexpr float kVRest      = -70.0f; // resting potential (mV)
inline constexpr float kVReset     = -70.0f; // post-spike reset potential (mV)
inline constexpr float kVThreshold = -55.0f; // spike threshold (mV)
inline constexpr float kRMembrane  = 10.0f;  // membrane resistance (MOhm), scales I_syn
inline constexpr float kVFloor     = -90.0f; // hard physiological clamp floor (mV)
inline constexpr float kVCeil      = 0.0f;   // hard physiological clamp ceiling (mV)
inline constexpr float kPerSpikePj = 0.9f;   // MODELED energy per spike (pJ), not measured HW

// ---- Network structures (fields consumed directly by the renderer) ---------
struct Neuron {
    int   id;
    float x, y;          // normalized [0,1] layout position for the topology view
    float membrane_v;    // real membrane potential in mV
    float threshold;     // spike threshold in mV
    bool  spiking;       // true on the step the neuron fired
    int   layer;
    std::string label;
};

struct Synapse {
    int   pre_id;
    int   post_id;
    float weight;        // synaptic weight (excitatory > 0, inhibitory < 0)
    float stdp_delta;    // last STDP weight change applied
    bool  active;
};

struct SNNConfig {
    int   num_layers = 3;
    std::array<int,4> layer_sizes = {8, 16, 8, 4};
    float dt_ms = 1.0f;
    int   sim_steps = 1000;
    float base_threshold = 1.0f;   // legacy slider (unused by LIF mV model directly)
    float tau_m_ms = 20.0f;        // membrane time constant (ms)
    float tau_s_ms = 5.0f;         // synaptic time constant (ms)
    bool  stdp_enabled = true;
    float stdp_ap = 0.01f;         // LTP amplitude
    float stdp_ad = 0.012f;        // LTD amplitude
    float input_rate_hz = 50.0f;   // Poisson drive rate into the first layer
};

struct SimMetrics {
    float spike_rate_hz;
    float avg_membrane_v;
    float energy_pj;
    float latency_ms;
    int   total_spikes;
    float accuracy;               // 0 unless a real rate-coded task is wired
    float spike_history[256];
    int   spike_history_offset;
};

// ---- Deterministic PRNG (xorshift32) so the harness is reproducible --------
struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed = 0x1234567u) : s(seed ? seed : 1u) {}
    uint32_t next_u32() {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return s;
    }
    float uniform() { return (next_u32() >> 8) * (1.0f / 16777216.0f); } // [0,1)
};

// ---- Engine ----------------------------------------------------------------
// Holds the per-step working state (synaptic current accumulators, spike
// bookkeeping). The Neuron/Synapse vectors live in the owning SNNState; the
// engine operates on references to them.
struct SnnEngine {
    Rng rng;

    // Per-neuron incoming synaptic current for the *current* step (mV/ms units
    // after R scaling), and the accumulator filled by spikes for the next step.
    std::vector<float> i_syn;        // applied this step
    std::vector<float> i_syn_next;   // accumulating for next step
    std::vector<double> last_spike_ms; // last spike time per neuron (for STDP)
    std::vector<float> input_layer_ids; // neuron ids in layer 0 (Poisson targets)

    // Outgoing synapse index per neuron, for fast current propagation.
    std::vector<std::vector<int>> out_syn; // out_syn[pre] = list of synapse idx

    double sim_time_ms = 0.0;
    int    step_count  = 0;

    void reset(size_t n_neurons, size_t /*n_synapses*/,
               const std::vector<Synapse>& synapses,
               const std::vector<int>& input_ids) {
        i_syn.assign(n_neurons, 0.0f);
        i_syn_next.assign(n_neurons, 0.0f);
        last_spike_ms.assign(n_neurons, -1.0e9);
        out_syn.assign(n_neurons, {});
        for (int si = 0; si < (int)synapses.size(); ++si) {
            const Synapse& sy = synapses[si];
            if (sy.pre_id >= 0 && sy.pre_id < (int)n_neurons)
                out_syn[sy.pre_id].push_back(si);
        }
        input_layer_ids.assign(input_ids.begin(), input_ids.end());
        sim_time_ms = 0.0;
        step_count  = 0;
    }

    // Advance the network by one dt. Returns number of spikes emitted this step.
    // Mutates neurons[].membrane_v / .spiking and synapses[].stdp_delta.
    int step(std::vector<Neuron>& neurons,
             std::vector<Synapse>& synapses,
             const SNNConfig& cfg,
             float dt_ms) {
        const size_t N = neurons.size();
        if (i_syn.size() != N) {
            // (re)derive input layer ids from layer==0
            std::vector<int> ids;
            for (auto& n : neurons) if (n.layer == 0) ids.push_back(n.id);
            reset(N, synapses.size(), synapses, ids);
        }

        // Swap accumulated current into the applied buffer for this step.
        i_syn.swap(i_syn_next);
        std::fill(i_syn_next.begin(), i_syn_next.end(), 0.0f);

        // Poisson input drive into layer 0: probability p = rate*dt over dt_ms.
        const float p_in = cfg.input_rate_hz * (dt_ms / 1000.0f);
        const float input_kick = 18.0f; // depolarizing input current (mV/ms equiv)
        for (int id : input_layer_ids) {
            if (rng.uniform() < p_in) i_syn[id] += input_kick;
        }

        const float tau = cfg.tau_m_ms > 0.1f ? cfg.tau_m_ms : 0.1f;
        int spikes_this_step = 0;

        // Integrate every neuron, detect threshold crossings.
        std::vector<int> fired;
        fired.reserve(8);
        for (size_t k = 0; k < N; ++k) {
            Neuron& nn = neurons[k];
            nn.spiking = false;
            float V = nn.membrane_v;
            float I = kRMembrane * i_syn[nn.id];
            float dV = (kVRest - V + I) / tau * dt_ms;
            V += dV;
            if (V > kVCeil)  V = kVCeil;
            if (V < kVFloor) V = kVFloor;

            if (V >= nn.threshold) {
                nn.spiking = true;
                nn.membrane_v = kVReset; // reset immediately below threshold
                last_spike_ms[nn.id] = sim_time_ms;
                fired.push_back((int)k);
                ++spikes_this_step;
            } else {
                nn.membrane_v = V;
            }
        }

        // Propagate weighted current from fired neurons to post neurons (next step)
        // and apply STDP on the synapses involved.
        for (int k : fired) {
            int pre = neurons[k].id;
            for (int si : out_syn[pre]) {
                Synapse& sy = synapses[si];
                if (!sy.active) continue;
                i_syn_next[sy.post_id] += sy.weight;

                if (cfg.stdp_enabled) {
                    // Pre fired now. If post fired recently before -> LTD (depress).
                    double dt_post = sim_time_ms - last_spike_ms[sy.post_id];
                    if (dt_post >= 0.0 && dt_post < 5.0 * cfg.tau_s_ms) {
                        float dw = -cfg.stdp_ad * std::exp(-(float)dt_post / cfg.tau_s_ms);
                        sy.weight += dw;
                        sy.stdp_delta = dw;
                    }
                }
            }
        }
        // Post-side LTP: for synapses whose post just fired and pre fired recently.
        if (cfg.stdp_enabled) {
            for (int k : fired) {
                int post = neurons[k].id;
                for (int si = 0; si < (int)synapses.size(); ++si) {
                    Synapse& sy = synapses[si];
                    if (!sy.active || sy.post_id != post) continue;
                    double dt_pre = sim_time_ms - last_spike_ms[sy.pre_id];
                    if (dt_pre > 0.0 && dt_pre < 5.0 * cfg.tau_s_ms) {
                        float dw = cfg.stdp_ap * std::exp(-(float)dt_pre / cfg.tau_s_ms);
                        sy.weight += dw;
                        sy.stdp_delta = dw;
                    }
                }
            }
        }

        sim_time_ms += dt_ms;
        ++step_count;
        return spikes_this_step;
    }
};

// ---- Network builder -------------------------------------------------------
// Builds a layered feed-forward network from cfg into neurons/synapses, with
// deterministic weights (no rand()). layout x/y are normalized for the view.
inline void build_network(const SNNConfig& cfg,
                          std::vector<Neuron>& neurons,
                          std::vector<Synapse>& synapses,
                          std::vector<int>& input_ids,
                          uint32_t seed = 0xA17C0DEu) {
    neurons.clear();
    synapses.clear();
    input_ids.clear();

    int nl = cfg.num_layers;
    if (nl < 1) nl = 1;
    if (nl > 4) nl = 4;

    std::vector<int> sizes;
    for (int l = 0; l < nl; ++l) {
        int c = cfg.layer_sizes[l];
        if (c < 1) c = 1;
        sizes.push_back(c);
    }

    Rng rng(seed);
    std::vector<int> layer_off(nl, 0);
    int id = 0;
    for (int l = 0; l < nl; ++l) {
        layer_off[l] = id;
        int count = sizes[l];
        for (int n = 0; n < count; ++n) {
            Neuron neuron;
            neuron.id = id++;
            neuron.x = ((float)l + 0.5f) / (float)nl;
            neuron.y = ((float)n + 0.5f) / (float)count;
            // start at rest with small deterministic jitter so the field isn't flat
            neuron.membrane_v = kVRest + (rng.uniform() - 0.5f) * 4.0f;
            neuron.threshold  = kVThreshold;
            neuron.spiking    = false;
            neuron.layer      = l;
            char lbl[16];
            std::snprintf(lbl, sizeof(lbl), "L%dN%d", l, n);
            neuron.label = lbl;
            neurons.push_back(neuron);
            if (l == 0) input_ids.push_back(neuron.id);
        }
    }

    // Dense-ish feed-forward synapses between consecutive layers.
    for (int l = 0; l + 1 < nl; ++l) {
        int src_count = sizes[l];
        int dst_count = sizes[l + 1];
        int src_off = layer_off[l];
        int dst_off = layer_off[l + 1];
        for (int s = 0; s < src_count; ++s) {
            for (int d = 0; d < dst_count; ++d) {
                // ~70% connectivity, deterministic
                if (rng.uniform() > 0.70f) continue;
                Synapse syn;
                syn.pre_id  = src_off + s;
                syn.post_id = dst_off + d;
                // Mostly excitatory weights sized to drive downstream firing.
                float w = 6.0f + rng.uniform() * 6.0f;       // 6..12 mV excitatory
                if (rng.uniform() < 0.15f) w = -(2.0f + rng.uniform() * 3.0f); // some inhibitory
                syn.weight  = w;
                syn.stdp_delta = 0.0f;
                syn.active  = true;
                synapses.push_back(syn);
            }
        }
    }
}

} // namespace straylight::snn
