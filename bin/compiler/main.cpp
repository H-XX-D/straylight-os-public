// bin/compiler/main.cpp
//
// straylight-compiler — on-demand graph optimization and lowering tool.
//
// Usage:
//   straylight-compiler optimize <input.json> [-o output.json]
//   straylight-compiler lower <input.json> --backend=<cpu|cuda|rocm>
//   straylight-compiler cache --clear

#include "cache.h"
#include "ir/graph.h"
#include "ir/lowering.h"
#include "ir/passes.h"

#include <straylight/error.h>
#include <straylight/log.h>
#include <straylight/result.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>

using namespace straylight;
using namespace straylight::compiler;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static const char* CACHE_DIR = "/var/cache/straylight/compiler";

static void print_usage() {
    std::cerr
        << "Usage: straylight-compiler <command> [options]\n"
        << "\n"
        << "Commands:\n"
        << "  optimize <input.json> [-o output.json]        Load graph, run passes, save\n"
        << "  lower <input.json> --backend=<cpu|cuda|rocm>  Lower to target backend IR\n"
        << "  cache --clear                                 Clear compilation cache\n"
        << "\n"
        << "Options:\n"
        << "  -h, --help    Show this help message\n";
}

static Result<std::string, std::string> read_file(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        return Result<std::string, std::string>::error("cannot open file: " + path);
    }
    std::ostringstream ss;
    ss << ifs.rdbuf();
    if (ifs.bad()) {
        return Result<std::string, std::string>::error("failed to read file: " + path);
    }
    return Result<std::string, std::string>::ok(ss.str());
}

static Result<void, std::string> write_file(const std::string& path,
                                             const std::string& content) {
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        return Result<void, std::string>::error("cannot create file: " + path);
    }
    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!ofs) {
        return Result<void, std::string>::error("failed to write file: " + path);
    }
    return Result<void, std::string>::ok();
}

static SLError make_error(SLErrorCode code, const std::string& msg) {
    return SLError{code, msg};
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

static Result<int, SLError> cmd_optimize(int argc, char* argv[]) {
    // Parse: optimize <input.json> [-o output.json]
    if (argc < 3) {
        return Result<int, SLError>::error(
            make_error(SLErrorCode::InvalidArgument,
                       "optimize requires an input file"));
    }

    std::string input_path = argv[2];
    std::string output_path;

    for (int i = 3; i < argc; i++) {
        if (std::strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_path = argv[++i];
        }
    }

    // Read input graph.
    auto file_res = read_file(input_path);
    if (!file_res.has_value()) {
        return Result<int, SLError>::error(
            make_error(SLErrorCode::IOError, file_res.error()));
    }

    // Deserialize.
    auto graph_res = Graph::deserialize_json(file_res.value());
    if (!graph_res.has_value()) {
        return Result<int, SLError>::error(
            make_error(SLErrorCode::ParseError, graph_res.error()));
    }
    Graph graph = std::move(graph_res).value();

    SL_INFO("loaded graph with {} nodes", graph.node_count());

    // Build and run passes.
    PassManager pm;
    pm.add_pass("fuse_matmul_relu", fuse_matmul_relu);
    pm.add_pass("constant_fold", constant_fold);
    pm.add_pass("eliminate_dead_nodes", eliminate_dead_nodes);

    auto pass_res = pm.run_all(graph);
    if (!pass_res.has_value()) {
        return Result<int, SLError>::error(
            make_error(SLErrorCode::Internal, pass_res.error()));
    }

    SL_INFO("optimization complete: {} passes made modifications",
            pass_res.value());
    SL_INFO("graph now has {} nodes", graph.node_count());

    // Serialize result.
    auto ser_res = graph.serialize_json();
    if (!ser_res.has_value()) {
        return Result<int, SLError>::error(
            make_error(SLErrorCode::Internal, ser_res.error()));
    }

    if (output_path.empty()) {
        // Write to stdout.
        std::cout << ser_res.value() << "\n";
    } else {
        auto wr = write_file(output_path, ser_res.value());
        if (!wr.has_value()) {
            return Result<int, SLError>::error(
                make_error(SLErrorCode::IOError, wr.error()));
        }
        SL_INFO("wrote optimized graph to {}", output_path);
    }

    return Result<int, SLError>::ok(0);
}

static Result<int, SLError> cmd_lower(int argc, char* argv[]) {
    // Parse: lower <input.json> --backend=<cpu|cuda|rocm>
    if (argc < 3) {
        return Result<int, SLError>::error(
            make_error(SLErrorCode::InvalidArgument,
                       "lower requires an input file"));
    }

    std::string input_path = argv[2];
    std::string backend_str;

    for (int i = 3; i < argc; i++) {
        std::string arg = argv[i];
        if (arg.rfind("--backend=", 0) == 0) {
            backend_str = arg.substr(10);
        }
    }

    if (backend_str.empty()) {
        return Result<int, SLError>::error(
            make_error(SLErrorCode::InvalidArgument,
                       "lower requires --backend=<cpu|cuda|rocm>"));
    }

    auto be_res = backend_from_string(backend_str);
    if (!be_res.has_value()) {
        return Result<int, SLError>::error(
            make_error(SLErrorCode::InvalidArgument, be_res.error()));
    }
    Backend backend = be_res.value();

    // Read input graph.
    auto file_res = read_file(input_path);
    if (!file_res.has_value()) {
        return Result<int, SLError>::error(
            make_error(SLErrorCode::IOError, file_res.error()));
    }

    auto graph_res = Graph::deserialize_json(file_res.value());
    if (!graph_res.has_value()) {
        return Result<int, SLError>::error(
            make_error(SLErrorCode::ParseError, graph_res.error()));
    }
    Graph graph = std::move(graph_res).value();

    SL_INFO("lowering {} nodes to {}", graph.node_count(),
            backend_to_string(backend));

    // Lower.
    Lowerer lowerer;
    auto ir_res = lowerer.lower(graph, backend);
    if (!ir_res.has_value()) {
        return Result<int, SLError>::error(
            make_error(SLErrorCode::Internal, ir_res.error()));
    }

    std::cout << ir_res.value();

    // Cache the result.
    auto ser_res = graph.serialize_json();
    if (ser_res.has_value()) {
        // Use a simple hash of the serialized graph as cache key.
        std::hash<std::string> hasher;
        std::string cache_key = std::to_string(hasher(ser_res.value())) +
                                "-" + backend_to_string(backend);

        CompilationCache cache(CACHE_DIR);
        auto put_res = cache.put(cache_key, ir_res.value());
        if (put_res.has_value()) {
            SL_DEBUG("cached lowered IR under key {}", cache_key);
        }
    }

    return Result<int, SLError>::ok(0);
}

static Result<int, SLError> cmd_cache(int argc, char* argv[]) {
    bool do_clear = false;

    for (int i = 2; i < argc; i++) {
        if (std::strcmp(argv[i], "--clear") == 0) {
            do_clear = true;
        }
    }

    CompilationCache cache(CACHE_DIR);

    if (do_clear) {
        size_t before = cache.size();
        cache.clear();
        SL_INFO("cleared {} cache entries", before);
        std::cout << "Cleared " << before << " cache entries.\n";
    } else {
        std::cout << "Cache entries: " << cache.size() << "\n";
        std::cout << "Cache directory: " << CACHE_DIR << "\n";
    }

    return Result<int, SLError>::ok(0);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    Log::init("straylight-compiler");

    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string command = argv[1];

    if (command == "-h" || command == "--help") {
        print_usage();
        return 0;
    }

    Result<int, SLError> result = Result<int, SLError>::ok(0);

    if (command == "optimize") {
        result = cmd_optimize(argc, argv);
    } else if (command == "lower") {
        result = cmd_lower(argc, argv);
    } else if (command == "cache") {
        result = cmd_cache(argc, argv);
    } else {
        std::cerr << "unknown command: " << command << "\n\n";
        print_usage();
        return 1;
    }

    if (!result.has_value()) {
        SL_ERROR("{}: {}", static_cast<int>(result.error().code()),
                 result.error().message());
        std::cerr << "error: " << result.error().message() << "\n";
        return static_cast<int>(result.error().code());
    }

    return result.value();
}
