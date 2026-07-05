// bin/snn/simulator.h
#pragma once

#include "network.h"

#include <string>
#include <vector>

#include <straylight/result.h>

namespace straylight::snn {

struct SimConfig {
    float dt = 0.1f;           // timestep in ms
    float duration = 100.0f;   // simulation duration in ms
    std::string output_path;   // path to write spike train CSV (optional)
};

struct SimResult {
    /// spike_trains[t][n] = true if neuron n spiked at timestep t
    std::vector<std::vector<bool>> spike_trains;
    float sim_time_ms;     // wall-clock simulation time (approx)
    size_t total_spikes;   // total number of spikes across all neurons and timesteps
};

/// High-level simulation runner.
class Simulator {
public:
    /// Run a simulation on the given network.
    /// input_currents[t][n] = current injected into neuron n at timestep t.
    /// If input_currents has fewer timesteps than needed, missing steps get zero current.
    straylight::Result<SimResult, std::string> run(
        Network& net,
        const SimConfig& cfg,
        const std::vector<std::vector<float>>& input_currents);

private:
    /// Write spike trains to CSV file.
    static straylight::Result<void, std::string> write_spike_csv(
        const std::string& path,
        const std::vector<std::vector<bool>>& spike_trains,
        float dt);
};

} // namespace straylight::snn
