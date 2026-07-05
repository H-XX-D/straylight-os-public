// tests/unit/subsystems/test_snn.cpp
#include <gtest/gtest.h>

#include "neuron.h"
#include "network.h"
#include "plasticity.h"
#include "simulator.h"

using namespace straylight::snn;

// ── LIF Neuron tests ────────────────────────────────────────────────────────

TEST(LIFNeuron, InitialState) {
    LIFNeuron neuron;
    EXPECT_FLOAT_EQ(neuron.voltage(), -65.0f);
    EXPECT_FALSE(neuron.spiked());
}

TEST(LIFNeuron, SubthresholdNoSpike) {
    LIFNeuron neuron;
    // Small current should not cause a spike
    for (int i = 0; i < 100; ++i) {
        neuron.step(0.1f, 0.1f);  // very small current
    }
    // Voltage should settle near rest + R_m * I_ext = -65 + 10 * 0.1 = -64
    EXPECT_FALSE(neuron.spiked());
    EXPECT_GT(neuron.voltage(), -66.0f);
    EXPECT_LT(neuron.voltage(), -60.0f);
}

TEST(LIFNeuron, SuprathresholdSpike) {
    LIFNeuron neuron;
    // Large current should cause a spike
    bool spiked = false;
    for (int i = 0; i < 1000; ++i) {
        neuron.step(0.1f, 5.0f);  // strong current
        if (neuron.spiked()) {
            spiked = true;
            break;
        }
    }
    EXPECT_TRUE(spiked);
    // After spike, voltage should be at reset
    EXPECT_FLOAT_EQ(neuron.voltage(), -70.0f);
}

TEST(LIFNeuron, RefractoryPeriod) {
    NeuronParams params;
    params.tau_ref = 5.0f;  // 5ms refractory
    LIFNeuron neuron(params);

    // Drive to spike
    while (!neuron.spiked()) {
        neuron.step(0.1f, 10.0f);
    }

    // During refractory period, voltage should stay at reset
    for (int i = 0; i < 30; ++i) {  // 3ms < 5ms refractory
        neuron.step(0.1f, 10.0f);
        EXPECT_FLOAT_EQ(neuron.voltage(), params.v_reset);
        EXPECT_FALSE(neuron.spiked());
    }
}

TEST(LIFNeuron, Reset) {
    LIFNeuron neuron;
    // Drive voltage up
    for (int i = 0; i < 50; ++i) {
        neuron.step(0.1f, 3.0f);
    }
    EXPECT_NE(neuron.voltage(), -65.0f);

    neuron.reset();
    EXPECT_FLOAT_EQ(neuron.voltage(), -65.0f);
    EXPECT_FALSE(neuron.spiked());
}

// ── Network tests ───────────────────────────────────────────────────────────

TEST(Network, AddNeurons) {
    Network net;
    auto id0 = net.add_neuron();
    auto id1 = net.add_neuron();
    EXPECT_EQ(id0, 0u);
    EXPECT_EQ(id1, 1u);
    EXPECT_EQ(net.neuron_count(), 2u);
}

TEST(Network, SpikePropagation) {
    // Two neurons: neuron 0 -> neuron 1 with strong weight
    Network net;
    net.add_neuron();  // neuron 0
    net.add_neuron();  // neuron 1
    net.add_synapse({0, 1, 5.0f, 0.5f});  // strong synapse, 0.5ms delay

    // Drive neuron 0 with strong current until it spikes
    bool n0_spiked = false;
    bool n1_spiked = false;

    for (int t = 0; t < 2000; ++t) {
        // Only inject current into neuron 0
        net.step(0.1f, {10.0f, 0.0f});

        const auto& spikes = net.spike_record();
        if (spikes.size() >= 1 && spikes[0]) n0_spiked = true;
        if (spikes.size() >= 2 && spikes[1]) n1_spiked = true;

        if (n1_spiked) break;
    }

    EXPECT_TRUE(n0_spiked) << "Neuron 0 should spike from direct current";
    EXPECT_TRUE(n1_spiked) << "Neuron 1 should spike from synaptic input";
}

TEST(Network, SpikeRecordSize) {
    Network net;
    net.add_neuron();
    net.add_neuron();
    net.add_neuron();

    net.step(0.1f, {0.0f, 0.0f, 0.0f});

    EXPECT_EQ(net.spike_record().size(), 3u);
}

// ── STDP tests ──────────────────────────────────────────────────────────────

TEST(STDP, PreBeforePostPotentiation) {
    Network net;
    net.add_neuron();
    net.add_neuron();
    net.add_synapse({0, 1, 1.0f, 1.0f});

    float initial_weight = net.synapses()[0].weight;

    // Pre fires at t=10ms, post fires at t=15ms (dt = +5ms -> potentiation)
    std::vector<float> pre_times = {10.0f, -1.0f};
    std::vector<float> post_times = {-1.0f, 15.0f};

    STDP stdp;
    stdp.update(net, pre_times, post_times);

    EXPECT_GT(net.synapses()[0].weight, initial_weight)
        << "Pre-before-post should strengthen synapse";
}

TEST(STDP, PostBeforePreDepression) {
    Network net;
    net.add_neuron();
    net.add_neuron();
    net.add_synapse({0, 1, 1.0f, 1.0f});

    float initial_weight = net.synapses()[0].weight;

    // Post fires at t=10ms, pre fires at t=15ms (dt = -5ms -> depression)
    std::vector<float> pre_times = {15.0f, -1.0f};
    std::vector<float> post_times = {-1.0f, 10.0f};

    STDP stdp;
    stdp.update(net, pre_times, post_times);

    EXPECT_LT(net.synapses()[0].weight, initial_weight)
        << "Post-before-pre should weaken synapse";
}

TEST(STDP, WeightClamp) {
    Network net;
    net.add_neuron();
    net.add_neuron();
    net.add_synapse({0, 1, 9.99f, 1.0f});

    STDPParams params;
    params.w_max = 10.0f;
    params.a_plus = 1.0f;  // very large potentiation

    // Pre before post, tiny dt -> large dW
    std::vector<float> pre_times = {10.0f, -1.0f};
    std::vector<float> post_times = {-1.0f, 10.1f};

    STDP stdp(params);
    stdp.update(net, pre_times, post_times);

    EXPECT_LE(net.synapses()[0].weight, params.w_max)
        << "Weight should be clamped to w_max";
}

TEST(STDP, NoSpikeNoChange) {
    Network net;
    net.add_neuron();
    net.add_neuron();
    net.add_synapse({0, 1, 1.0f, 1.0f});

    float initial_weight = net.synapses()[0].weight;

    // Neither neuron has spiked
    std::vector<float> pre_times = {-1.0f, -1.0f};
    std::vector<float> post_times = {-1.0f, -1.0f};

    STDP stdp;
    stdp.update(net, pre_times, post_times);

    EXPECT_FLOAT_EQ(net.synapses()[0].weight, initial_weight);
}

// ── Simulator tests ─────────────────────────────────────────────────────────

TEST(Simulator, BasicRun) {
    Network net;
    net.add_neuron();
    net.add_neuron();
    net.add_synapse({0, 1, 3.0f, 1.0f});

    SimConfig cfg;
    cfg.dt = 0.1f;
    cfg.duration = 50.0f;

    // Inject current into neuron 0
    size_t steps = static_cast<size_t>(cfg.duration / cfg.dt);
    std::vector<std::vector<float>> inputs(steps, std::vector<float>{5.0f, 0.0f});

    Simulator sim;
    auto result = sim.run(net, cfg, inputs);
    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(result.value().spike_trains.size(), steps);
    EXPECT_GT(result.value().total_spikes, 0u);
    EXPECT_GT(result.value().sim_time_ms, 0.0f);
}

TEST(Simulator, EmptyNetworkError) {
    Network net;
    SimConfig cfg;
    cfg.dt = 0.1f;
    cfg.duration = 10.0f;

    Simulator sim;
    auto result = sim.run(net, cfg, {});
    EXPECT_FALSE(result.has_value());
}

TEST(Simulator, InvalidDtError) {
    Network net;
    net.add_neuron();

    SimConfig cfg;
    cfg.dt = 0.0f;
    cfg.duration = 10.0f;

    Simulator sim;
    auto result = sim.run(net, cfg, {});
    EXPECT_FALSE(result.has_value());
}
