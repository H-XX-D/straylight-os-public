// bin/snn/simulator.cpp
#include "simulator.h"

#include <chrono>
#include <fstream>
#include <sstream>

namespace straylight::snn {

straylight::Result<void, std::string> Simulator::write_spike_csv(
    const std::string& path,
    const std::vector<std::vector<bool>>& spike_trains,
    float dt) {

    std::ofstream ofs(path);
    if (!ofs) {
        return straylight::Result<void, std::string>::error(
            "Cannot open output file: " + path);
    }

    // Header
    if (!spike_trains.empty()) {
        ofs << "time_ms";
        for (size_t n = 0; n < spike_trains[0].size(); ++n) {
            ofs << ",neuron_" << n;
        }
        ofs << "\n";

        // Data rows
        for (size_t t = 0; t < spike_trains.size(); ++t) {
            ofs << (static_cast<float>(t) * dt);
            for (size_t n = 0; n < spike_trains[t].size(); ++n) {
                ofs << "," << (spike_trains[t][n] ? 1 : 0);
            }
            ofs << "\n";
        }
    }

    if (!ofs) {
        return straylight::Result<void, std::string>::error(
            "Failed writing spike train CSV");
    }
    return straylight::Result<void, std::string>::ok();
}

straylight::Result<SimResult, std::string> Simulator::run(
    Network& net,
    const SimConfig& cfg,
    const std::vector<std::vector<float>>& input_currents) {

    if (cfg.dt <= 0.0f) {
        return straylight::Result<SimResult, std::string>::error(
            "Timestep dt must be positive");
    }
    if (cfg.duration <= 0.0f) {
        return straylight::Result<SimResult, std::string>::error(
            "Duration must be positive");
    }
    if (net.neuron_count() == 0) {
        return straylight::Result<SimResult, std::string>::error(
            "Network has no neurons");
    }

    size_t num_steps = static_cast<size_t>(cfg.duration / cfg.dt);
    size_t num_neurons = net.neuron_count();

    SimResult result;
    result.spike_trains.reserve(num_steps);
    result.total_spikes = 0;

    auto wall_start = std::chrono::steady_clock::now();

    for (size_t t = 0; t < num_steps; ++t) {
        // Get input currents for this timestep, or empty vector
        const std::vector<float>& currents =
            (t < input_currents.size()) ? input_currents[t]
                                        : std::vector<float>{};

        net.step(cfg.dt, currents);

        // Record spikes
        const auto& spikes = net.spike_record();
        result.spike_trains.push_back(spikes);

        for (size_t n = 0; n < num_neurons; ++n) {
            if (n < spikes.size() && spikes[n]) {
                ++result.total_spikes;
            }
        }
    }

    auto wall_end = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        wall_end - wall_start);
    result.sim_time_ms = static_cast<float>(elapsed.count()) / 1000.0f;

    // Optionally write spike trains to CSV
    if (!cfg.output_path.empty()) {
        auto wr = write_spike_csv(cfg.output_path, result.spike_trains, cfg.dt);
        if (!wr.has_value()) {
            return straylight::Result<SimResult, std::string>::error(wr.error());
        }
    }

    return straylight::Result<SimResult, std::string>::ok(std::move(result));
}

} // namespace straylight::snn
