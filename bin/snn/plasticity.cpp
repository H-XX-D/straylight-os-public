// bin/snn/plasticity.cpp
#include "plasticity.h"

#include <algorithm>
#include <cmath>

namespace straylight::snn {

STDP::STDP(STDPParams params) : params_(params) {}

void STDP::update(Network& net,
                  const std::vector<float>& spike_times_pre,
                  const std::vector<float>& spike_times_post) {

    auto& synapses = net.synapses();

    for (auto& syn : synapses) {
        uint32_t pre = syn.pre;
        uint32_t post = syn.post;

        // Check bounds
        if (pre >= spike_times_pre.size() || post >= spike_times_post.size()) {
            continue;
        }

        float t_pre = spike_times_pre[pre];
        float t_post = spike_times_post[post];

        // Skip if either neuron has never spiked (indicated by negative time)
        if (t_pre < 0.0f || t_post < 0.0f) {
            continue;
        }

        float dt = t_post - t_pre;
        float dw = 0.0f;

        if (dt > 0.0f) {
            // Pre fires before post -> LTP (potentiation)
            dw = params_.a_plus * std::exp(-dt / params_.tau_plus);
        } else if (dt < 0.0f) {
            // Post fires before pre -> LTD (depression)
            dw = -params_.a_minus * std::exp(dt / params_.tau_minus);
        }
        // dt == 0: no change

        syn.weight += dw;
        syn.weight = std::clamp(syn.weight, params_.w_min, params_.w_max);
    }
}

const STDPParams& STDP::params() const {
    return params_;
}

} // namespace straylight::snn
