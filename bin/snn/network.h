// bin/snn/network.h
#pragma once

#include "neuron.h"

#include <cstdint>
#include <string>
#include <vector>

namespace straylight::snn {

struct Synapse {
    uint32_t pre;     // presynaptic neuron index
    uint32_t post;    // postsynaptic neuron index
    float weight;     // synaptic weight (nA injected per spike)
    float delay;      // conduction delay in ms
};

/// A network of LIF neurons connected by synapses.
class Network {
public:
    /// Add a neuron and return its index.
    uint32_t add_neuron(NeuronParams p = {});

    /// Add a synapse between two neurons.
    void add_synapse(Synapse s);

    /// Step the entire network by dt ms.
    /// external_currents[i] = current injected into neuron i this timestep.
    /// If external_currents is shorter than neuron_count(), missing entries default to 0.
    void step(float dt, const std::vector<float>& external_currents);

    /// After a step, returns the spike state for each neuron.
    [[nodiscard]] const std::vector<bool>& spike_record() const;

    /// Number of neurons in the network.
    [[nodiscard]] size_t neuron_count() const;

    /// Access neurons (for STDP weight updates etc.)
    [[nodiscard]] std::vector<LIFNeuron>& neurons();
    [[nodiscard]] const std::vector<LIFNeuron>& neurons() const;

    /// Access synapses (mutable for learning rules).
    [[nodiscard]] std::vector<Synapse>& synapses();
    [[nodiscard]] const std::vector<Synapse>& synapses() const;

private:
    std::vector<LIFNeuron> neurons_;
    std::vector<Synapse> synapses_;
    std::vector<bool> spikes_;
    float current_time_ = 0.0f;

    /// Pending spike deliveries: (delivery_time, post_neuron, weight)
    struct PendingSpike {
        float delivery_time;
        uint32_t post;
        float weight;
    };
    std::vector<PendingSpike> pending_;
};

} // namespace straylight::snn
