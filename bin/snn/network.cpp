// bin/snn/network.cpp
#include "network.h"

#include <algorithm>

namespace straylight::snn {

uint32_t Network::add_neuron(NeuronParams p) {
    uint32_t idx = static_cast<uint32_t>(neurons_.size());
    neurons_.emplace_back(p);
    return idx;
}

void Network::add_synapse(Synapse s) {
    synapses_.push_back(s);
}

void Network::step(float dt, const std::vector<float>& external_currents) {
    size_t n = neurons_.size();
    spikes_.assign(n, false);

    // Collect synaptic currents from pending spike deliveries
    std::vector<float> syn_currents(n, 0.0f);

    // Deliver pending spikes whose time has arrived
    auto it = pending_.begin();
    while (it != pending_.end()) {
        if (current_time_ >= it->delivery_time) {
            if (it->post < n) {
                syn_currents[it->post] += it->weight;
            }
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }

    // Combine external and synaptic currents, then step each neuron
    for (size_t i = 0; i < n; ++i) {
        float i_ext = (i < external_currents.size()) ? external_currents[i] : 0.0f;
        i_ext += syn_currents[i];

        neurons_[i].step(dt, i_ext);
        spikes_[i] = neurons_[i].spiked();
    }

    // For each neuron that spiked, queue pending deliveries through synapses
    for (const auto& syn : synapses_) {
        if (syn.pre < n && spikes_[syn.pre]) {
            float delivery_time = current_time_ + syn.delay;
            pending_.push_back({delivery_time, syn.post, syn.weight});
        }
    }

    current_time_ += dt;
}

const std::vector<bool>& Network::spike_record() const {
    return spikes_;
}

size_t Network::neuron_count() const {
    return neurons_.size();
}

std::vector<LIFNeuron>& Network::neurons() {
    return neurons_;
}

const std::vector<LIFNeuron>& Network::neurons() const {
    return neurons_;
}

std::vector<Synapse>& Network::synapses() {
    return synapses_;
}

const std::vector<Synapse>& Network::synapses() const {
    return synapses_;
}

} // namespace straylight::snn
