// bin/quantum/main.cpp
// straylight-quantum — Quantum gate simulator
// Usage: straylight-quantum <simulate|measure|benchmark> [options]

#include "circuit.h"
#include "gates.h"
#include "measure.h"
#include "noise.h"
#include "state_vector.h"

#include <straylight/error.h>
#include <straylight/result.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

using namespace straylight;
using namespace straylight::quantum;

static void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " <command> [options]\n\n"
              << "Commands:\n"
              << "  simulate   --qubits <N> --circuit <gate-list>\n"
              << "             [--noise-depol <p>] [--noise-deph <p>] [--noise-damp <g>]\n"
              << "  measure    --qubits <N> --circuit <gate-list> --shots <S>\n"
              << "  benchmark  --qubits <N> [--depth <D>]\n"
              << "  vqe        --model <tfim|heisenberg> [--J <f>] [--h <f>]   (ground state via Solver)\n";
}

static std::string find_arg(int argc, char* argv[], const std::string& flag) {
    for (int i = 0; i < argc - 1; ++i) {
        if (argv[i] == flag) return argv[i + 1];
    }
    return "";
}

/// Parse a simple gate list: "h:0,cx:0:1,rz:1:3.14"
static Result<Circuit, std::string> parse_circuit(const std::string& spec) {
    Circuit circuit;
    if (spec.empty()) {
        return Result<Circuit, std::string>::ok(std::move(circuit));
    }

    std::istringstream iss(spec);
    std::string token;
    while (std::getline(iss, token, ',')) {
        // Format: name:qubit[:qubit][:param]
        std::istringstream tss(token);
        std::string part;
        std::vector<std::string> parts;
        while (std::getline(tss, part, ':')) {
            parts.push_back(part);
        }
        if (parts.size() < 2) {
            return Result<Circuit, std::string>::error(
                "Invalid gate spec '" + token + "': need at least name:qubit");
        }

        std::string name = parts[0];
        std::vector<uint32_t> qubits;
        std::vector<double> params;

        // Determine how many qubit indices vs params based on gate name.
        bool is_two_qubit = (name == "cx" || name == "CX" || name == "cnot" ||
                             name == "CNOT" || name == "swap" || name == "SWAP");
        size_t num_qubit_args = is_two_qubit ? 2 : 1;

        for (size_t i = 1; i < parts.size(); ++i) {
            if (i <= num_qubit_args) {
                qubits.push_back(static_cast<uint32_t>(std::stoul(parts[i])));
            } else {
                params.push_back(std::stod(parts[i]));
            }
        }

        circuit.add_gate(name, std::move(qubits), std::move(params));
    }
    return Result<Circuit, std::string>::ok(std::move(circuit));
}

static Result<void, SLError> cmd_simulate(int argc, char* argv[]) {
    std::string qubits_str = find_arg(argc, argv, "--qubits");
    std::string circuit_str = find_arg(argc, argv, "--circuit");
    std::string depol_str = find_arg(argc, argv, "--noise-depol");
    std::string deph_str = find_arg(argc, argv, "--noise-deph");
    std::string damp_str = find_arg(argc, argv, "--noise-damp");

    uint32_t nq = qubits_str.empty() ? 3 : static_cast<uint32_t>(std::stoul(qubits_str));

    StateVector sv(nq);

    auto circuit_res = parse_circuit(circuit_str);
    if (!circuit_res.has_value()) {
        return Result<void, SLError>::error(
            {SLErrorCode::InvalidArgument, circuit_res.error()});
    }

    auto exec = circuit_res.value().execute(sv);
    if (!exec.has_value()) {
        return Result<void, SLError>::error({SLErrorCode::Internal, exec.error()});
    }

    // Apply noise if requested.
    NoiseModel noise;
    if (!depol_str.empty()) noise.add_depolarizing(std::stod(depol_str));
    if (!deph_str.empty()) noise.add_dephasing(std::stod(deph_str));
    if (!damp_str.empty()) noise.add_amplitude_damping(std::stod(damp_str));

    if (noise.channel_count() > 0) {
        for (uint32_t q = 0; q < nq; ++q) {
            auto nr = noise.apply(sv, q);
            if (!nr.has_value()) {
                return Result<void, SLError>::error({SLErrorCode::Internal, nr.error()});
            }
        }
    }

    // Print state vector.
    auto probs = sv.probabilities();
    std::cout << "State vector (" << nq << " qubits, " << sv.dim() << " amplitudes):\n";
    for (size_t i = 0; i < sv.dim(); ++i) {
        if (probs[i] > 1e-10) {
            std::cout << "  |";
            for (int b = nq - 1; b >= 0; --b) {
                std::cout << ((i >> b) & 1);
            }
            std::cout << "> : " << sv[i].real();
            if (std::abs(sv[i].imag()) > 1e-10) {
                std::cout << (sv[i].imag() >= 0 ? "+" : "") << sv[i].imag() << "i";
            }
            std::cout << "  (p=" << probs[i] << ")\n";
        }
    }

    return Result<void, SLError>::ok();
}

static Result<void, SLError> cmd_measure(int argc, char* argv[]) {
    std::string qubits_str = find_arg(argc, argv, "--qubits");
    std::string circuit_str = find_arg(argc, argv, "--circuit");
    std::string shots_str = find_arg(argc, argv, "--shots");

    uint32_t nq = qubits_str.empty() ? 3 : static_cast<uint32_t>(std::stoul(qubits_str));
    uint32_t shots = shots_str.empty() ? 1000 : static_cast<uint32_t>(std::stoul(shots_str));

    StateVector sv(nq);

    auto circuit_res = parse_circuit(circuit_str);
    if (!circuit_res.has_value()) {
        return Result<void, SLError>::error(
            {SLErrorCode::InvalidArgument, circuit_res.error()});
    }

    auto exec = circuit_res.value().execute(sv);
    if (!exec.has_value()) {
        return Result<void, SLError>::error({SLErrorCode::Internal, exec.error()});
    }

    auto results = Measurement::measure_all(sv, shots);
    if (!results.has_value()) {
        return Result<void, SLError>::error({SLErrorCode::Internal, results.error()});
    }

    // Build histogram.
    std::vector<uint32_t> histogram(sv.dim(), 0);
    for (auto r : results.value()) {
        histogram[r]++;
    }

    std::cout << "Measurement results (" << shots << " shots, " << nq << " qubits):\n";
    for (size_t i = 0; i < histogram.size(); ++i) {
        if (histogram[i] > 0) {
            std::cout << "  |";
            for (int b = nq - 1; b >= 0; --b) {
                std::cout << ((i >> b) & 1);
            }
            std::cout << "> : " << histogram[i]
                      << " (" << (100.0 * histogram[i] / shots) << "%)\n";
        }
    }

    return Result<void, SLError>::ok();
}

static Result<void, SLError> cmd_benchmark(int argc, char* argv[]) {
    std::string qubits_str = find_arg(argc, argv, "--qubits");
    std::string depth_str = find_arg(argc, argv, "--depth");

    uint32_t nq = qubits_str.empty() ? 10 : static_cast<uint32_t>(std::stoul(qubits_str));
    uint32_t depth = depth_str.empty() ? 20 : static_cast<uint32_t>(std::stoul(depth_str));

    std::cout << "Benchmarking: " << nq << " qubits, depth " << depth << "\n";

    StateVector sv(nq);
    Circuit circuit;

    // Build a random-ish circuit: alternating Hadamard + CNOT layers.
    for (uint32_t d = 0; d < depth; ++d) {
        // Hadamard layer on all qubits.
        for (uint32_t q = 0; q < nq; ++q) {
            circuit.add_gate("h", {q});
        }
        // CNOT layer on adjacent pairs.
        for (uint32_t q = 0; q + 1 < nq; q += 2) {
            circuit.add_gate("cx", {q, q + 1});
        }
    }

    auto start = std::chrono::high_resolution_clock::now();
    auto exec = circuit.execute(sv);
    auto end = std::chrono::high_resolution_clock::now();

    if (!exec.has_value()) {
        return Result<void, SLError>::error({SLErrorCode::Internal, exec.error()});
    }

    double elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();
    std::cout << "Circuit: " << circuit.gate_count() << " gates\n";
    std::cout << "State dim: " << sv.dim() << "\n";
    std::cout << "Time: " << elapsed_ms << " ms\n";
    std::cout << "Gates/sec: " << (circuit.gate_count() / (elapsed_ms / 1000.0)) << "\n";

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

// Find the ground state of a 2-qubit Hamiltonian via the StrayLight Solver winch
// (non-secret pre-B300 optimizer), shelling out to straylight-quantum-vqe. The
// tool checks itself against numpy exact diagonalization.
static Result<void, SLError> cmd_vqe(int argc, char* argv[]) {
    std::string model = find_arg(argc, argv, "--model");
    std::string J = find_arg(argc, argv, "--J");
    std::string h = find_arg(argc, argv, "--h");
    if (model.empty()) model = "tfim";
    if (J.empty()) J = "1.0";
    if (h.empty()) h = "1.0";
    if (model != "tfim" && model != "heisenberg")
        return Result<void, SLError>::error({SLErrorCode::Internal, "model must be tfim or heisenberg"});
    if (!is_number(J) || !is_number(h))
        return Result<void, SLError>::error({SLErrorCode::Internal, "J and h must be numbers"});
    std::string cmd = "straylight-quantum-vqe --model " + model + " --J " + J + " --h " + h + " --json";
    int rc = std::system(cmd.c_str());
    if (rc != 0)
        return Result<void, SLError>::error({SLErrorCode::Internal,
            "vqe failed (need straylight-quantum-vqe + Solver; set SL_SOLVER_DIR)"});
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
    } else if (cmd == "measure") {
        result = cmd_measure(argc, argv);
    } else if (cmd == "benchmark") {
        result = cmd_benchmark(argc, argv);
    } else if (cmd == "vqe") {
        result = cmd_vqe(argc, argv);
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
