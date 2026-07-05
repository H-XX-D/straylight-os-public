// bin/snn/neuron.h
#pragma once

#include <cstdint>

namespace straylight::snn {

struct NeuronParams {
    float v_rest = -65.0f;       // mV, resting potential
    float v_threshold = -55.0f;  // mV, spike threshold
    float v_reset = -70.0f;      // mV, post-spike reset
    float tau_m = 10.0f;         // ms, membrane time constant
    float tau_ref = 2.0f;        // ms, refractory period
    float r_m = 10.0f;           // MOhm, membrane resistance
};

/// Leaky Integrate-and-Fire neuron model.
///
/// Dynamics: dV/dt = (-(V - V_rest) + R_m * I_ext) / tau_m
/// When V >= V_threshold: spike, then V = V_reset, enter refractory period.
class LIFNeuron {
public:
    explicit LIFNeuron(NeuronParams params = {});

    /// Advance the neuron by one timestep of dt milliseconds.
    /// i_ext is the external input current in nA.
    void step(float dt, float i_ext);

    /// Returns true if the neuron spiked on the most recent step.
    [[nodiscard]] bool spiked() const;

    /// Returns the current membrane voltage.
    [[nodiscard]] float voltage() const;

    /// Reset the neuron to its initial state.
    void reset();

    /// Get neuron parameters.
    [[nodiscard]] const NeuronParams& params() const;

private:
    NeuronParams params_;
    float v_;               // current membrane voltage
    bool spiked_;           // did we spike on the last step?
    float ref_remaining_;   // remaining refractory time in ms
};

} // namespace straylight::snn
