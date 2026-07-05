// bin/snn/main.cpp
// straylight-snn — Spiking neural network simulator
// Usage: straylight-snn <simulate|create-network> [options]

#include "network.h"
#include "neuron.h"
#include "plasticity.h"
#include "simulator.h"

#include <straylight/error.h>
#include <straylight/result.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

using namespace straylight;
using namespace straylight::snn;

static void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " <command> [options]\n\n"
              << "Commands:\n"
              << "  simulate        --neurons <N> --duration <ms> --dt <ms>\n"
              << "                  [--output <csv>] [--current <nA>] [--stdp]\n"
              << "  create-network  --neurons <N> --connectivity <float>\n"
              << "                  --output <json> [--weight <float>] [--delay <float>]\n"
              << "  fit             --target-rate <Hz> [--tau <ms>] [--vth <v>]   (LIF current via Solver)\n";
}

static std::string find_arg(int argc, char* argv[], const std::string& flag) {
    for (int i = 0; i < argc - 1; ++i) {
        if (argv[i] == flag) return argv[i + 1];
    }
    return "";
}

static bool has_flag(int argc, char* argv[], const std::string& flag) {
    for (int i = 0; i < argc; ++i) {
        if (argv[i] == flag) return true;
    }
    return false;
}

static Result<void, SLError> cmd_simulate(int argc, char* argv[]) {
    std::string neurons_str = find_arg(argc, argv, "--neurons");
    std::string duration_str = find_arg(argc, argv, "--duration");
    std::string dt_str = find_arg(argc, argv, "--dt");
    std::string output = find_arg(argc, argv, "--output");
    std::string current_str = find_arg(argc, argv, "--current");
    bool use_stdp = has_flag(argc, argv, "--stdp");

    uint32_t num_neurons = neurons_str.empty() ? 10 : static_cast<uint32_t>(std::stoul(neurons_str));
    float duration = duration_str.empty() ? 100.0f : std::stof(duration_str);
    float dt = dt_str.empty() ? 0.1f : std::stof(dt_str);
    float current = current_str.empty() ? 2.0f : std::stof(current_str);

    // Build a simple network: N neurons, chain connectivity
    Network net;
    for (uint32_t i = 0; i < num_neurons; ++i) {
        net.add_neuron();
    }

    // Connect each neuron to the next in a chain
    for (uint32_t i = 0; i + 1 < num_neurons; ++i) {
        net.add_synapse({i, i + 1, 1.5f, 1.0f});
    }

    // Build input currents: inject constant current into neuron 0
    size_t num_steps = static_cast<size_t>(duration / dt);
    std::vector<std::vector<float>> input_currents(num_steps);
    for (size_t t = 0; t < num_steps; ++t) {
        input_currents[t].resize(num_neurons, 0.0f);
        input_currents[t][0] = current;
    }

    SimConfig cfg;
    cfg.dt = dt;
    cfg.duration = duration;
    cfg.output_path = output;

    Simulator sim;
    auto result = sim.run(net, cfg, input_currents);
    if (!result.has_value()) {
        return Result<void, SLError>::error(
            {SLErrorCode::Internal, result.error()});
    }

    // Optionally apply STDP after simulation
    if (use_stdp) {
        // Gather last spike times from spike trains
        std::vector<float> last_spike(num_neurons, -1.0f);
        for (size_t t = 0; t < result.value().spike_trains.size(); ++t) {
            for (size_t n = 0; n < num_neurons; ++n) {
                if (result.value().spike_trains[t][n]) {
                    last_spike[n] = static_cast<float>(t) * dt;
                }
            }
        }

        STDP stdp;
        stdp.update(net, last_spike, last_spike);
        std::cout << "STDP applied to " << net.synapses().size() << " synapses\n";
    }

    std::cout << "Simulation complete: " << result.value().total_spikes
              << " total spikes in " << result.value().sim_time_ms << " ms wall time\n";

    if (!output.empty()) {
        std::cout << "Spike trains written to " << output << "\n";
    }

    return Result<void, SLError>::ok();
}

static Result<void, SLError> cmd_create_network(int argc, char* argv[]) {
    std::string neurons_str = find_arg(argc, argv, "--neurons");
    std::string conn_str = find_arg(argc, argv, "--connectivity");
    std::string output = find_arg(argc, argv, "--output");
    std::string weight_str = find_arg(argc, argv, "--weight");
    std::string delay_str = find_arg(argc, argv, "--delay");

    if (output.empty()) {
        return Result<void, SLError>::error(
            {SLErrorCode::InvalidArgument, "create-network requires --output"});
    }

    uint32_t num_neurons = neurons_str.empty() ? 10 : static_cast<uint32_t>(std::stoul(neurons_str));
    float connectivity = conn_str.empty() ? 0.3f : std::stof(conn_str);
    float weight = weight_str.empty() ? 1.0f : std::stof(weight_str);
    float delay = delay_str.empty() ? 1.0f : std::stof(delay_str);

    if (connectivity < 0.0f || connectivity > 1.0f) {
        return Result<void, SLError>::error(
            {SLErrorCode::InvalidArgument, "Connectivity must be in [0, 1]"});
    }

    // Generate network description as JSON
    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"num_neurons\": " << num_neurons << ",\n";
    oss << "  \"neuron_params\": {\n";
    oss << "    \"v_rest\": -65.0, \"v_threshold\": -55.0, \"v_reset\": -70.0,\n";
    oss << "    \"tau_m\": 10.0, \"tau_ref\": 2.0, \"r_m\": 10.0\n";
    oss << "  },\n";
    oss << "  \"synapses\": [\n";

    // Create random connectivity using a simple deterministic pattern
    // (stride-based for reproducibility)
    bool first = true;
    uint32_t stride = std::max(1u, static_cast<uint32_t>(1.0f / connectivity));
    for (uint32_t i = 0; i < num_neurons; ++i) {
        for (uint32_t j = 0; j < num_neurons; ++j) {
            if (i == j) continue;
            // Deterministic "random" connectivity based on stride
            if ((i * num_neurons + j) % stride == 0) {
                if (!first) oss << ",\n";
                first = false;
                oss << "    {\"pre\": " << i << ", \"post\": " << j
                    << ", \"weight\": " << weight << ", \"delay\": " << delay << "}";
            }
        }
    }
    oss << "\n  ]\n";
    oss << "}\n";

    std::ofstream outfile(output);
    if (!outfile) {
        return Result<void, SLError>::error(
            {SLErrorCode::IOError, "Cannot open output file: " + output});
    }
    outfile << oss.str();
    if (!outfile) {
        return Result<void, SLError>::error(
            {SLErrorCode::IOError, "Failed to write network file"});
    }

    std::cout << "Network config written to " << output
              << " (" << num_neurons << " neurons)\n";
    return Result<void, SLError>::ok();
}

// Reject anything that is not a plain number, so the validated value is safe to
// pass to the shell below (prevents command injection via the args).
static bool is_number(const std::string& s) {
    if (s.empty()) return false;
    char* end = nullptr;
    std::strtod(s.c_str(), &end);
    return end && *end == '\0';
}

// Fit the LIF input current to a target firing rate via the StrayLight Solver
// winch (non-secret pre-B300 optimizer), shelling out to straylight-snn-fit.
static Result<void, SLError> cmd_fit(int argc, char* argv[]) {
    std::string rate = find_arg(argc, argv, "--target-rate");
    std::string tau  = find_arg(argc, argv, "--tau");
    std::string vth  = find_arg(argc, argv, "--vth");
    if (rate.empty()) rate = "50";
    if (tau.empty())  tau  = "10";
    if (vth.empty())  vth  = "1.0";
    if (!is_number(rate) || !is_number(tau) || !is_number(vth))
        return Result<void, SLError>::error({SLErrorCode::Internal, "target-rate, tau, vth must be numbers"});
    std::string cmd = "straylight-snn-fit --target-rate " + rate + " --tau " + tau + " --vth " + vth + " --json";
    int rc = std::system(cmd.c_str());
    if (rc != 0)
        return Result<void, SLError>::error({SLErrorCode::Internal,
            "fit failed (need straylight-snn-fit + Solver; set SL_SOLVER_DIR)"});
    return Result<void, SLError>::ok();
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string cmd = argv[1];
    Result<void, SLError> result = Result<void, SLError>::ok();

    if (cmd == "simulate") {
        result = cmd_simulate(argc, argv);
    } else if (cmd == "create-network") {
        result = cmd_create_network(argc, argv);
    } else if (cmd == "fit") {
        result = cmd_fit(argc, argv);
    } else if (cmd == "--help" || cmd == "-h") {
        print_usage(argv[0]);
        return 0;
    } else {
        std::cerr << "Unknown command: " << cmd << "\n";
        print_usage(argv[0]);
        return 1;
    }

    if (!result.has_value()) {
        std::cerr << "Error [" << static_cast<int>(result.error().code())
                  << "]: " << result.error().message() << "\n";
        return static_cast<int>(result.error().code());
    }

    return 0;
}
