// bin/enclave/main.cpp
// straylight-enclave — SGX secure inference tool
//
// Usage:
//   straylight-enclave attest   [--stub]
//   straylight-enclave seal     --input <file> --output <file> [--policy mrenclave|mrsigner]
//   straylight-enclave unseal   --input <file> --output <file>
//   straylight-enclave infer    --model <sealed-model> [--input <file>]
//   straylight-enclave info

#include "attestation.h"
#include "secure_inference.h"
#include "sealed_storage.h"

#include <straylight/config.h>
#include <straylight/error.h>
#include <straylight/log.h>
#include <straylight/result.h>

#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <filesystem>
#include <span>
#include <sstream>
#include <string>
#include <vector>

using namespace straylight;
using namespace straylight::enclave;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void print_usage(const char* prog) {
    std::cerr
        << "Usage: " << prog << " <command> [options]\n\n"
        << "Commands:\n"
        << "  attest              Generate and verify an attestation report\n"
        << "    [--stub]          Force stub mode (default when SGX unavailable)\n\n"
        << "  seal                Seal a file using the enclave key\n"
        << "    --input  <file>   Plaintext input file\n"
        << "    --output <file>   Sealed output file\n"
        << "    [--policy mrenclave|mrsigner]  Default: mrenclave\n\n"
        << "  unseal              Unseal a previously sealed file\n"
        << "    --input  <file>   Sealed input file\n"
        << "    --output <file>   Plaintext output file\n\n"
        << "  infer               Run secure model inference\n"
        << "    --model <file>    Sealed model file (produced by 'seal')\n"
        << "    [--input <file>]  Raw input tensor file (stdin if omitted)\n\n"
        << "  info                Print enclave capability information\n";
}

/// Return the value of a named flag from argv, or empty string.
static std::string arg_value(int argc, char* argv[], const std::string& flag) {
    for (int i = 1; i < argc - 1; ++i) {
        if (argv[i] == flag) return argv[i + 1];
    }
    return {};
}

static bool has_flag(int argc, char* argv[], const std::string& flag) {
    for (int i = 1; i < argc; ++i) {
        if (argv[i] == flag) return true;
    }
    return false;
}

static std::string bytes_to_hex(const std::vector<uint8_t>& v) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (auto b : v) oss << std::setw(2) << static_cast<unsigned>(b);
    return oss.str();
}

static Result<std::vector<uint8_t>, SLError> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        return Result<std::vector<uint8_t>, SLError>::error(
            {SLErrorCode::IOError, "Cannot open file: " + path});
    }
    return Result<std::vector<uint8_t>, SLError>::ok(
        std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                             std::istreambuf_iterator<char>{}));
}

static Result<void, SLError> write_file(const std::string& path,
                                        const std::vector<uint8_t>& data) {
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) {
        return Result<void, SLError>::error(
            {SLErrorCode::IOError, "Cannot write file: " + path});
    }
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
    if (!f.good()) {
        return Result<void, SLError>::error(
            {SLErrorCode::IOError, "Write failed: " + path});
    }
    return Result<void, SLError>::ok();
}

// ---------------------------------------------------------------------------
// Command implementations
// ---------------------------------------------------------------------------

static Result<void, SLError> cmd_attest(int argc, char* argv[]) {
    SgxMode mode = has_flag(argc, argv, "--stub") ? SgxMode::Stub : SgxMode::Stub;
#ifdef STRAYLIGHT_SGX_HW
    if (!has_flag(argc, argv, "--stub")) mode = SgxMode::Hardware;
#endif

    AttestationCtx ctx;
    if (auto r = ctx.init(mode); !r.has_value()) {
        return Result<void, SLError>::error({SLErrorCode::HardwareFault, r.error()});
    }

    auto report = ctx.generate_local_report();
    if (!report.has_value()) {
        return Result<void, SLError>::error({SLErrorCode::Internal, report.error()});
    }

    std::cout << "SGX Local Attestation Report\n"
              << "  Mode:       " << (mode == SgxMode::Hardware ? "hardware" : "stub") << "\n"
              << "  MRENCLAVE:  " << report.value().mr_enclave << "\n"
              << "  MRSIGNER:   " << report.value().mr_signer  << "\n"
              << "  ISV SVN:    " << report.value().isv_svn    << "\n"
              << "  ReportData: "
              << bytes_to_hex(report.value().report_data).substr(0, 32) << "...\n";

    auto quote = ctx.generate_remote_quote(report.value());
    if (!quote.has_value()) {
        return Result<void, SLError>::error({SLErrorCode::Internal, quote.error()});
    }
    std::cout << "  Quote bytes: " << quote.value().data.size() << "\n"
              << "  EPID group:  " << quote.value().epid_group_id << "\n";

    auto ok = ctx.verify_quote(quote.value());
    if (!ok.has_value()) {
        return Result<void, SLError>::error({SLErrorCode::Internal, ok.error()});
    }
    std::cout << "  Verified:    " << (ok.value() ? "YES" : "NO") << "\n";

    return Result<void, SLError>::ok();
}

static Result<void, SLError> cmd_seal(int argc, char* argv[]) {
    const std::string input_path  = arg_value(argc, argv, "--input");
    const std::string output_path = arg_value(argc, argv, "--output");
    const std::string policy_str  = arg_value(argc, argv, "--policy");

    if (input_path.empty() || output_path.empty()) {
        return Result<void, SLError>::error(
            {SLErrorCode::InvalidArgument, "seal requires --input and --output"});
    }

    SealPolicy policy = (policy_str == "mrsigner")
                        ? SealPolicy::MrSigner
                        : SealPolicy::MrEnclave;

    auto data = read_file(input_path);
    if (!data.has_value()) return Result<void, SLError>::error(data.error());

    SealedStore store;
    if (auto r = store.init(SgxMode::Stub); !r.has_value()) {
        return Result<void, SLError>::error({SLErrorCode::Internal, r.error()});
    }

    auto blob = store.seal(std::span<const uint8_t>(data.value()), policy);
    if (!blob.has_value()) {
        return Result<void, SLError>::error({SLErrorCode::Internal, blob.error()});
    }

    // Serialise blob to file: [nonce:12][tag:16][ciphertext:N]
    std::vector<uint8_t> out;
    out.insert(out.end(), blob.value().nonce.begin(), blob.value().nonce.end());
    out.insert(out.end(), blob.value().tag.begin(), blob.value().tag.end());
    out.insert(out.end(), blob.value().ciphertext.begin(), blob.value().ciphertext.end());

    auto w = write_file(output_path, out);
    if (!w.has_value()) return Result<void, SLError>::error(w.error());

    std::cout << "Sealed " << data.value().size() << " bytes -> "
              << out.size() << " bytes\n"
              << "Policy:  "
              << (policy == SealPolicy::MrEnclave ? "MRENCLAVE" : "MRSIGNER") << "\n"
              << "Written: " << output_path << "\n";

    return Result<void, SLError>::ok();
}

static Result<void, SLError> cmd_unseal(int argc, char* argv[]) {
    const std::string input_path  = arg_value(argc, argv, "--input");
    const std::string output_path = arg_value(argc, argv, "--output");

    if (input_path.empty() || output_path.empty()) {
        return Result<void, SLError>::error(
            {SLErrorCode::InvalidArgument, "unseal requires --input and --output"});
    }

    auto raw = read_file(input_path);
    if (!raw.has_value()) return Result<void, SLError>::error(raw.error());

    constexpr size_t NONCE = 12, TAG = 16, MIN = NONCE + TAG;
    if (raw.value().size() < MIN) {
        return Result<void, SLError>::error(
            {SLErrorCode::ParseError, "Input file too small to be a sealed blob"});
    }

    SealedBlob blob;
    blob.nonce.assign(raw.value().begin(), raw.value().begin() + NONCE);
    blob.tag.assign(raw.value().begin() + NONCE, raw.value().begin() + MIN);
    blob.ciphertext.assign(raw.value().begin() + MIN, raw.value().end());
    blob.policy = SealPolicy::MrEnclave;

    SealedStore store;
    if (auto r = store.init(SgxMode::Stub); !r.has_value()) {
        return Result<void, SLError>::error({SLErrorCode::Internal, r.error()});
    }

    auto plaintext = store.unseal(blob);
    if (!plaintext.has_value()) {
        return Result<void, SLError>::error(
            {SLErrorCode::PermissionDenied, plaintext.error()});
    }

    auto w = write_file(output_path, plaintext.value());
    if (!w.has_value()) return Result<void, SLError>::error(w.error());

    std::cout << "Unsealed " << raw.value().size() << " bytes -> "
              << plaintext.value().size() << " bytes\n"
              << "Written: " << output_path << "\n";

    return Result<void, SLError>::ok();
}

static Result<void, SLError> cmd_infer(int argc, char* argv[]) {
    const std::string model_path = arg_value(argc, argv, "--model");
    const std::string input_path = arg_value(argc, argv, "--input");

    if (model_path.empty()) {
        return Result<void, SLError>::error(
            {SLErrorCode::InvalidArgument, "infer requires --model"});
    }

    std::vector<uint8_t> input_data;
    if (!input_path.empty()) {
        auto d = read_file(input_path);
        if (!d.has_value()) return Result<void, SLError>::error(d.error());
        input_data = std::move(d.value());
    } else {
        // Read raw bytes from stdin.
        std::istreambuf_iterator<char> it(std::cin), end;
        input_data.assign(it, end);
    }
    if (input_data.empty()) {
        return Result<void, SLError>::error(
            {SLErrorCode::InvalidArgument, "Input tensor is empty"});
    }

    // Use a default Config with sane defaults.
    auto cfg_result = straylight::Config::load("/etc/straylight/enclave.conf");
    // If config is missing, fall back to empty JSON.
    // We need a Config object — create one from a minimal JSON string.
    // Since Config::load returns Result<Config,string>, we need a fallback.
    // Build a temporary file with empty JSON when the config is missing.
    straylight::Config cfg = cfg_result.has_value()
        ? std::move(cfg_result.value())
        : []() -> straylight::Config {
            // Load a minimal empty config from an in-memory path trick.
            // Write a temp file since Config only reads from file.
            namespace fs = std::filesystem;
            const fs::path tmp = fs::temp_directory_path() / "sl_enclave_defaults.json";
            {
                std::ofstream f(tmp);
                f << "{}";
            }
            auto r = straylight::Config::load(tmp);
            fs::remove(tmp);
            // If even that fails, we cannot proceed safely.
            if (!r.has_value()) throw std::runtime_error("cannot create default config");
            return r.value();
        }();

    SecureInference engine;
    if (auto r = engine.init(SgxMode::Stub, cfg); !r.has_value()) {
        return Result<void, SLError>::error({SLErrorCode::Internal, r.error()});
    }

    InferenceRequest req{
        .encrypted_model = model_path,
        .input_tensor    = std::move(input_data),
    };

    auto result = engine.run(req);
    if (!result.has_value()) {
        return Result<void, SLError>::error({SLErrorCode::Internal, result.error()});
    }

    std::cout << "Secure inference complete\n"
              << "  Latency:        " << result.value().latency_us << " us\n"
              << "  Output bytes:   " << result.value().encrypted_output.size() << "\n"
              << "  Output (hex):   "
              << bytes_to_hex(result.value().encrypted_output).substr(0, 64)
              << (result.value().encrypted_output.size() > 32 ? "..." : "") << "\n";

    return Result<void, SLError>::ok();
}

static Result<void, SLError> cmd_info() {
    const bool device_present = std::filesystem::exists("/dev/straylight-enclave");
    const bool sgx_link_present = std::filesystem::exists("/dev/straylight/sgx");
    bool cpu_sgx = false;
    {
        std::ifstream cpuinfo("/proc/cpuinfo");
        std::string line;
        while (std::getline(cpuinfo, line)) {
            if (line.rfind("flags", 0) == 0 &&
                line.find(" sgx") != std::string::npos) {
                cpu_sgx = true;
                break;
            }
        }
    }

    std::cout << "StrayLight Enclave Subsystem\n"
              << "  Version:    1.0\n"
#ifdef STRAYLIGHT_SGX_HW
              << "  SGX:        hardware (compiled)\n"
#else
              << "  SGX:        stub (software simulation)\n"
#endif
              << "  Kernel dev: " << (device_present ? "/dev/straylight-enclave" : "missing") << "\n"
              << "  SGX link:   " << (sgx_link_present ? "/dev/straylight/sgx" : "missing") << "\n"
              << "  CPU SGX:    " << (cpu_sgx ? "yes" : "no") << "\n"
              << "  Crypto:     AES-256-GCM (OpenSSL EVP)\n"
              << "  Seal key:   EGETKEY-derived (hardware) / fixed-stub\n"
              << "  Algorithms: LocalReport, RemoteQuote, DCAP/EPID stub\n";
    return Result<void, SLError>::ok();
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    straylight::Log::init("straylight-enclave");

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const std::string cmd = argv[1];
    Result<void, SLError> result = Result<void, SLError>::ok();

    if (cmd == "attest") {
        result = cmd_attest(argc, argv);
    } else if (cmd == "seal") {
        result = cmd_seal(argc, argv);
    } else if (cmd == "unseal") {
        result = cmd_unseal(argc, argv);
    } else if (cmd == "infer") {
        result = cmd_infer(argc, argv);
    } else if (cmd == "info") {
        result = cmd_info();
    } else if (cmd == "--help" || cmd == "-h") {
        print_usage(argv[0]);
        return 0;
    } else {
        std::cerr << "Unknown command: " << cmd << "\n";
        print_usage(argv[0]);
        return 1;
    }

    if (!result.has_value()) {
        SL_ERROR("[{}] {}", static_cast<int>(result.error().code()),
                 result.error().message());
        return static_cast<int>(result.error().code());
    }

    return 0;
}
