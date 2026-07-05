// bin/photonics/main.cpp
// straylight-photonics — Photonic mesh simulator
// Usage: straylight-photonics <simulate|program|detect> [options]

#include "detector.h"
#include "device.h"
#include "mesh.h"
#include "mzi.h"

#include <straylight/error.h>
#include <straylight/result.h>

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

using namespace straylight;
using namespace straylight::photonics;

static void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " <command> [options]\n\n"
              << "Commands:\n"
              << "  simulate  --rows <N> --cols <N> [--theta <rad>] [--phi <rad>]\n"
              << "  program   --rows <N> --cols <N> --device <path>\n"
              << "  detect    --rows <N> --cols <N> [--efficiency <0-1>] [--dark-rate <0-1>]\n"
              << "  decompose --target <dft|hadamard> [--modes <N>]   (synthesize mesh via Solver)\n";
}

static std::string find_arg(int argc, char* argv[], const std::string& flag) {
    for (int i = 0; i < argc - 1; ++i) {
        if (argv[i] == flag) return argv[i + 1];
    }
    return "";
}

static Result<void, SLError> cmd_simulate(int argc, char* argv[]) {
    std::string rows_str = find_arg(argc, argv, "--rows");
    std::string cols_str = find_arg(argc, argv, "--cols");
    std::string theta_str = find_arg(argc, argv, "--theta");
    std::string phi_str = find_arg(argc, argv, "--phi");

    uint32_t rows = rows_str.empty() ? 4 : static_cast<uint32_t>(std::stoul(rows_str));
    uint32_t cols = cols_str.empty() ? 4 : static_cast<uint32_t>(std::stoul(cols_str));
    double theta = theta_str.empty() ? M_PI / 4.0 : std::stod(theta_str);
    double phi = phi_str.empty() ? 0.0 : std::stod(phi_str);

    PhotonicMesh mesh;
    mesh.set_size(rows, cols);

    // Set all MZIs to the same theta/phi for demonstration.
    for (uint32_t c = 0; c < cols; ++c) {
        uint32_t offset = (c % 2 == 0) ? 0 : 1;
        for (uint32_t r = offset; r + 1 < rows; r += 2) {
            mesh.set_mzi(r, c, {theta, phi});
        }
    }

    // Input: all power in waveguide 0.
    std::vector<Complex> input(rows, {0.0, 0.0});
    input[0] = {1.0, 0.0};

    auto result = mesh.forward(input);
    if (!result.has_value()) {
        return Result<void, SLError>::error({SLErrorCode::Internal, result.error()});
    }

    std::cout << "Photonic mesh simulation (" << rows << "x" << cols << "):\n";
    std::cout << "MZI params: theta=" << theta << " phi=" << phi << "\n\n";

    const auto& output = result.value();
    double total_power = 0.0;
    for (size_t i = 0; i < output.size(); ++i) {
        double power = std::norm(output[i]);
        total_power += power;
        std::cout << "  Waveguide " << i << ": amplitude=("
                  << output[i].real() << ", " << output[i].imag()
                  << ") power=" << power << "\n";
    }
    std::cout << "\nTotal power: " << total_power << " (should be ~1.0)\n";

    return Result<void, SLError>::ok();
}

static Result<void, SLError> cmd_program(int argc, char* argv[]) {
    std::string rows_str = find_arg(argc, argv, "--rows");
    std::string cols_str = find_arg(argc, argv, "--cols");
    std::string device_str = find_arg(argc, argv, "--device");

    uint32_t rows = rows_str.empty() ? 4 : static_cast<uint32_t>(std::stoul(rows_str));
    uint32_t cols = cols_str.empty() ? 4 : static_cast<uint32_t>(std::stoul(cols_str));
    std::string device_path = device_str.empty() ? "/dev/photonic0" : device_str;

    PhotonicMesh mesh;
    mesh.set_size(rows, cols);

    // Identity mesh (all zeros) — no transformation.
    PhotonicDevice device;
    auto conn = device.connect(device_path);
    if (!conn.has_value()) {
        return Result<void, SLError>::error({SLErrorCode::IOError, conn.error()});
    }

    auto prog = device.program_mesh(mesh);
    if (!prog.has_value()) {
        return Result<void, SLError>::error({SLErrorCode::IOError, prog.error()});
    }

    std::cout << "Programmed " << rows << "x" << cols
              << " mesh to device " << device_path << "\n";

    // Run a test input.
    std::vector<Complex> input(rows, {0.0, 0.0});
    input[0] = {1.0, 0.0};

    auto run_result = device.run(input);
    if (!run_result.has_value()) {
        return Result<void, SLError>::error({SLErrorCode::Internal, run_result.error()});
    }

    std::cout << "Detection probabilities:\n";
    for (size_t i = 0; i < run_result.value().size(); ++i) {
        std::cout << "  Waveguide " << i << ": " << run_result.value()[i] << "\n";
    }

    return Result<void, SLError>::ok();
}

static Result<void, SLError> cmd_detect(int argc, char* argv[]) {
    std::string rows_str = find_arg(argc, argv, "--rows");
    std::string cols_str = find_arg(argc, argv, "--cols");
    std::string eff_str = find_arg(argc, argv, "--efficiency");
    std::string dark_str = find_arg(argc, argv, "--dark-rate");

    uint32_t rows = rows_str.empty() ? 4 : static_cast<uint32_t>(std::stoul(rows_str));
    uint32_t cols = cols_str.empty() ? 4 : static_cast<uint32_t>(std::stoul(cols_str));
    double efficiency = eff_str.empty() ? 0.95 : std::stod(eff_str);
    double dark_rate = dark_str.empty() ? 1e-6 : std::stod(dark_str);

    // Build a simple mesh and propagate.
    PhotonicMesh mesh;
    mesh.set_size(rows, cols);
    for (uint32_t c = 0; c < cols; ++c) {
        uint32_t offset = (c % 2 == 0) ? 0 : 1;
        for (uint32_t r = offset; r + 1 < rows; r += 2) {
            mesh.set_mzi(r, c, {M_PI / 4.0, 0.0});
        }
    }

    std::vector<Complex> input(rows, {0.0, 0.0});
    input[0] = {1.0, 0.0};

    auto fwd = mesh.forward(input);
    if (!fwd.has_value()) {
        return Result<void, SLError>::error({SLErrorCode::Internal, fwd.error()});
    }

    Detector det;
    auto probs = det.detect(fwd.value(), efficiency, dark_rate);
    if (!probs.has_value()) {
        return Result<void, SLError>::error({SLErrorCode::Internal, probs.error()});
    }

    auto clicks = det.sample_clicks(fwd.value(), efficiency, dark_rate);
    if (!clicks.has_value()) {
        return Result<void, SLError>::error({SLErrorCode::Internal, clicks.error()});
    }

    std::cout << "Detection (efficiency=" << efficiency << " dark_rate=" << dark_rate << "):\n";
    for (size_t i = 0; i < probs.value().size(); ++i) {
        std::cout << "  Waveguide " << i << ": prob=" << probs.value()[i]
                  << " click=" << clicks.value()[i] << "\n";
    }

    return Result<void, SLError>::ok();
}

static Result<void, SLError> cmd_decompose(int argc, char* argv[]) {
    std::string target = find_arg(argc, argv, "--target");
    std::string modes  = find_arg(argc, argv, "--modes");
    if (target.empty()) target = "dft";
    if (modes.empty())  modes  = "4";
    // Validate before shelling out (prevents injection); synthesis runs the
    // StrayLight Solver winch (L-BFGS-B) via straylight-photonics-decompose.
    if (target != "dft" && target != "hadamard")
        return Result<void, SLError>::error({SLErrorCode::Internal, "target must be dft or hadamard"});
    for (char ch : modes)
        if (!std::isdigit(static_cast<unsigned char>(ch)))
            return Result<void, SLError>::error({SLErrorCode::Internal, "modes must be a number"});
    std::string cmd = "straylight-photonics-decompose --target " + target + " --modes " + modes + " --json";
    int rc = std::system(cmd.c_str());
    if (rc != 0)
        return Result<void, SLError>::error({SLErrorCode::Internal,
            "decompose failed (need straylight-photonics-decompose + Solver; set SL_SOLVER_DIR)"});
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
    } else if (cmd == "program") {
        result = cmd_program(argc, argv);
    } else if (cmd == "detect") {
        result = cmd_detect(argc, argv);
    } else if (cmd == "decompose") {
        result = cmd_decompose(argc, argv);
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
