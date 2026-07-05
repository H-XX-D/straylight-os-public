// bin/snn/plasticity.h
#pragma once

#include "network.h"

#include <vector>

namespace straylight::snn {

struct STDPParams {
    float a_plus = 0.01f;    // LTP amplitude
    float a_minus = 0.012f;  // LTD amplitude
    float tau_plus = 20.0f;  // LTP time constant (ms)
    float tau_minus = 20.0f; // LTD time constant (ms)
    float w_max = 10.0f;     // maximum synaptic weight
    float w_min = -10.0f;    // minimum synaptic weight (negative for inhibitory)
};

/// Spike-Timing-Dependent Plasticity learning rule.
///
/// For each synapse (pre -> post):
///   If pre fires before post (dt = t_post - t_pre > 0):
///     dW = A+ * exp(-dt / tau+)    [potentiation]
///   If post fires before pre (dt < 0):
///     dW = -A- * exp(dt / tau-)    [depression]
class STDP {
public:
    explicit STDP(STDPParams params = {});

    /// Update all synapse weights in the network based on spike timing.
    /// spike_times_pre[i]  = time of last spike of neuron i as presynaptic (-1 if never)
    /// spike_times_post[i] = time of last spike of neuron i as postsynaptic (-1 if never)
    void update(Network& net,
                const std::vector<float>& spike_times_pre,
                const std::vector<float>& spike_times_post);

    /// Get params.
    [[nodiscard]] const STDPParams& params() const;

private:
    STDPParams params_;
};

} // namespace straylight::snn
