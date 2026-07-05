// bin/morph/main.cpp
// straylight-morph — Model transformation tool
// Usage: straylight-morph <quantize|prune|distill|adapt> [options]

#include "adapt.h"
#include "distill.h"
#include "prune.h"
#include "quantize.h"

#include <straylight/error.h>
#include <straylight/result.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace straylight;
using namespace straylight::morph;

static void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " <command> [options]\n\n"
              << "Commands:\n"
              << "  quantize  --input <file> --output <file> --mode <int8|int4|mixed>\n"
              << "            [--percentile <float>] [--per-channel]\n"
              << "  prune     --input <file> --output <file> --strategy <magnitude|structured|random>\n"
              << "            --sparsity <float> [--cols <int>] [--seed <int>]\n"
              << "  distill   --teacher <path> --student <path> --output <file>\n"
              << "            [--temperature <float>] [--alpha <float>]\n"
              << "  adapt     --output <file> --rank <int> --alpha <float>\n"
              << "            --modules <mod1,mod2,...> [--hidden-dim <int>] [--base-params <int>]\n";
}

static std::string find_arg(int argc, char* argv[], const std::string& flag) {
    for (int i = 0; i < argc - 1; ++i) {
        if (argv[i] == flag) {
            return argv[i + 1];
        }
    }
    return "";
}

static bool has_flag(int argc, char* argv[], const std::string& flag) {
    for (int i = 0; i < argc; ++i) {
        if (argv[i] == flag) return true;
    }
    return false;
}

/// Read a binary float vector from a file (raw float32 dump).
static Result<std::vector<float>, std::string> read_weights(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs) {
        return Result<std::vector<float>, std::string>::error(
            "Cannot open weight file: " + path);
    }

    auto size = ifs.tellg();
    if (size <= 0 || size % sizeof(float) != 0) {
        return Result<std::vector<float>, std::string>::error(
            "Invalid weight file size (must be multiple of 4 bytes): " + path);
    }

    ifs.seekg(0, std::ios::beg);
    size_t count = static_cast<size_t>(size) / sizeof(float);
    std::vector<float> weights(count);
    ifs.read(reinterpret_cast<char*>(weights.data()),
             static_cast<std::streamsize>(size));

    if (!ifs) {
        return Result<std::vector<float>, std::string>::error(
            "Failed to read weight data from: " + path);
    }

    return Result<std::vector<float>, std::string>::ok(std::move(weights));
}

/// Write quantized int8 data to a binary file.
static Result<void, std::string> write_quantized(const std::string& path,
                                                  const QuantResult& qr) {
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) {
        return Result<void, std::string>::error("Cannot open output file: " + path);
    }

    // Header: scale(4) + zero_point(4) + mode(4) + original_size(8) + data
    ofs.write(reinterpret_cast<const char*>(&qr.scale), sizeof(float));
    ofs.write(reinterpret_cast<const char*>(&qr.zero_point), sizeof(int32_t));
    uint32_t mode_val = static_cast<uint32_t>(qr.mode);
    ofs.write(reinterpret_cast<const char*>(&mode_val), sizeof(uint32_t));
    uint64_t orig = static_cast<uint64_t>(qr.original_size);
    ofs.write(reinterpret_cast<const char*>(&orig), sizeof(uint64_t));
    ofs.write(reinterpret_cast<const char*>(qr.quantized.data()),
              static_cast<std::streamsize>(qr.quantized.size()));

    if (!ofs) {
        return Result<void, std::string>::error("Failed to write output: " + path);
    }
    return Result<void, std::string>::ok();
}

/// Write pruned float weights to binary file.
static Result<void, std::string> write_pruned(const std::string& path,
                                               const PruneResult& pr) {
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) {
        return Result<void, std::string>::error("Cannot open output file: " + path);
    }

    ofs.write(reinterpret_cast<const char*>(pr.pruned.data()),
              static_cast<std::streamsize>(pr.pruned.size() * sizeof(float)));

    if (!ofs) {
        return Result<void, std::string>::error("Failed to write output: " + path);
    }
    return Result<void, std::string>::ok();
}

/// Write a JSON string to a text file.
static Result<void, std::string> write_json(const std::string& path,
                                             const std::string& json) {
    std::ofstream ofs(path);
    if (!ofs) {
        return Result<void, std::string>::error("Cannot open output file: " + path);
    }
    ofs << json << "\n";
    if (!ofs) {
        return Result<void, std::string>::error("Failed to write output: " + path);
    }
    return Result<void, std::string>::ok();
}

static Result<void, SLError> cmd_quantize(int argc, char* argv[]) {
    std::string input = find_arg(argc, argv, "--input");
    std::string output = find_arg(argc, argv, "--output");
    std::string mode_str = find_arg(argc, argv, "--mode");

    if (input.empty() || output.empty()) {
        return Result<void, SLError>::error(
            {SLErrorCode::InvalidArgument, "quantize requires --input and --output"});
    }

    QuantConfig cfg;
    if (mode_str == "int4") cfg.mode = QuantMode::Int4;
    else if (mode_str == "mixed") cfg.mode = QuantMode::Mixed;
    else cfg.mode = QuantMode::Int8;

    std::string pct = find_arg(argc, argv, "--percentile");
    if (!pct.empty()) cfg.calibration_percentile = std::stof(pct);

    cfg.per_channel = has_flag(argc, argv, "--per-channel");

    auto weights_r = read_weights(input);
    if (!weights_r.has_value()) {
        return Result<void, SLError>::error(
            {SLErrorCode::IOError, weights_r.error()});
    }

    Quantizer quantizer;
    auto qr = quantizer.quantize(weights_r.value(), cfg);
    if (!qr.has_value()) {
        return Result<void, SLError>::error(
            {SLErrorCode::Internal, qr.error()});
    }

    auto wr = write_quantized(output, qr.value());
    if (!wr.has_value()) {
        return Result<void, SLError>::error(
            {SLErrorCode::IOError, wr.error()});
    }

    std::cout << "Quantized " << weights_r.value().size() << " weights to "
              << output << " (mode=" << mode_str << ")\n";
    return Result<void, SLError>::ok();
}

static Result<void, SLError> cmd_prune(int argc, char* argv[]) {
    std::string input = find_arg(argc, argv, "--input");
    std::string output = find_arg(argc, argv, "--output");
    std::string strategy_str = find_arg(argc, argv, "--strategy");
    std::string sparsity_str = find_arg(argc, argv, "--sparsity");

    if (input.empty() || output.empty()) {
        return Result<void, SLError>::error(
            {SLErrorCode::InvalidArgument, "prune requires --input and --output"});
    }

    PruneConfig cfg;
    if (strategy_str == "structured") cfg.strategy = PruneStrategy::Structured;
    else if (strategy_str == "random") cfg.strategy = PruneStrategy::Random;
    else cfg.strategy = PruneStrategy::Magnitude;

    if (!sparsity_str.empty()) cfg.sparsity = std::stof(sparsity_str);

    std::string cols_str = find_arg(argc, argv, "--cols");
    if (!cols_str.empty()) cfg.structured_cols = static_cast<uint32_t>(std::stoul(cols_str));

    std::string seed_str = find_arg(argc, argv, "--seed");
    if (!seed_str.empty()) cfg.seed = std::stoull(seed_str);

    auto weights_r = read_weights(input);
    if (!weights_r.has_value()) {
        return Result<void, SLError>::error(
            {SLErrorCode::IOError, weights_r.error()});
    }

    Pruner pruner;
    auto pr = pruner.prune(weights_r.value(), cfg);
    if (!pr.has_value()) {
        return Result<void, SLError>::error(
            {SLErrorCode::Internal, pr.error()});
    }

    auto wr = write_pruned(output, pr.value());
    if (!wr.has_value()) {
        return Result<void, SLError>::error(
            {SLErrorCode::IOError, wr.error()});
    }

    std::cout << "Pruned " << weights_r.value().size() << " weights -> "
              << output << " (sparsity=" << pr.value().actual_sparsity << ")\n";
    return Result<void, SLError>::ok();
}

static Result<void, SLError> cmd_distill(int argc, char* argv[]) {
    std::string teacher = find_arg(argc, argv, "--teacher");
    std::string student = find_arg(argc, argv, "--student");
    std::string output = find_arg(argc, argv, "--output");

    if (teacher.empty() || student.empty() || output.empty()) {
        return Result<void, SLError>::error(
            {SLErrorCode::InvalidArgument,
             "distill requires --teacher, --student, and --output"});
    }

    DistillConfig cfg;
    cfg.teacher_path = teacher;
    cfg.student_path = student;

    std::string temp_str = find_arg(argc, argv, "--temperature");
    if (!temp_str.empty()) cfg.temperature = std::stof(temp_str);

    std::string alpha_str = find_arg(argc, argv, "--alpha");
    if (!alpha_str.empty()) cfg.alpha = std::stof(alpha_str);

    Distiller distiller;
    auto dr = distiller.prepare(cfg);
    if (!dr.has_value()) {
        return Result<void, SLError>::error(
            {SLErrorCode::Internal, dr.error()});
    }

    auto wr = write_json(output, dr.value().config_json);
    if (!wr.has_value()) {
        return Result<void, SLError>::error(
            {SLErrorCode::IOError, wr.error()});
    }

    std::cout << "Distillation config written to " << output << "\n";
    return Result<void, SLError>::ok();
}

static Result<void, SLError> cmd_adapt(int argc, char* argv[]) {
    std::string output = find_arg(argc, argv, "--output");
    std::string rank_str = find_arg(argc, argv, "--rank");
    std::string alpha_str = find_arg(argc, argv, "--alpha");
    std::string modules_str = find_arg(argc, argv, "--modules");

    if (output.empty() || modules_str.empty()) {
        return Result<void, SLError>::error(
            {SLErrorCode::InvalidArgument,
             "adapt requires --output and --modules"});
    }

    AdaptConfig cfg;
    if (!rank_str.empty()) cfg.rank = static_cast<uint32_t>(std::stoul(rank_str));
    if (!alpha_str.empty()) cfg.alpha = std::stof(alpha_str);

    // Parse comma-separated module names
    {
        std::istringstream ss(modules_str);
        std::string token;
        while (std::getline(ss, token, ',')) {
            if (!token.empty()) {
                cfg.target_modules.push_back(token);
            }
        }
    }

    std::string dim_str = find_arg(argc, argv, "--hidden-dim");
    if (!dim_str.empty()) cfg.hidden_dim = static_cast<uint32_t>(std::stoul(dim_str));

    std::string bp_str = find_arg(argc, argv, "--base-params");
    if (!bp_str.empty()) cfg.base_model_params = std::stoull(bp_str);

    Adapter adapter;
    auto ar = adapter.create_lora_config(cfg);
    if (!ar.has_value()) {
        return Result<void, SLError>::error(
            {SLErrorCode::Internal, ar.error()});
    }

    auto wr = write_json(output, ar.value().config_json);
    if (!wr.has_value()) {
        return Result<void, SLError>::error(
            {SLErrorCode::IOError, wr.error()});
    }

    std::cout << "LoRA config written to " << output
              << " (trainable=" << ar.value().trainable_params
              << ", ratio=" << ar.value().trainable_ratio << ")\n";
    return Result<void, SLError>::ok();
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string cmd = argv[1];
    Result<void, SLError> result = Result<void, SLError>::ok();

    if (cmd == "quantize") {
        result = cmd_quantize(argc, argv);
    } else if (cmd == "prune") {
        result = cmd_prune(argc, argv);
    } else if (cmd == "distill") {
        result = cmd_distill(argc, argv);
    } else if (cmd == "adapt") {
        result = cmd_adapt(argc, argv);
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
