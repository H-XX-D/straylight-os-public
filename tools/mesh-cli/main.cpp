// tools/mesh-cli/main.cpp
//
// straylight-mesh — CLI for the StrayLight GPU Mesh subsystem.
//
// Usage:
//   straylight-mesh pool                       Show all GPUs in mesh
//   straylight-mesh status                     Mesh health overview
//   straylight-mesh run <command> [--vram 8G]   Run on best available GPU
//   straylight-mesh transfer <src> <dst>        Move data between GPUs
//   straylight-mesh benchmark                   Measure mesh interconnect speeds
//   straylight-mesh help                        Show this help
//
// In production, this tool communicates with straylight-mesh daemon via D-Bus
// (org.straylight.Mesh1). On macOS without D-Bus, it prints what it would do.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void print_usage() {
    std::cerr
        << "Usage: straylight-mesh <command> [args...]\n"
        << "\n"
        << "Commands:\n"
        << "  pool                       Show all GPUs in mesh\n"
        << "  status                     Mesh health overview\n"
        << "  run <command> [--vram 8G]  Run command on best available GPU\n"
        << "  transfer <src> <dst>       Move data between GPUs (host:gpu format)\n"
        << "  benchmark                  Measure mesh interconnect speeds\n"
        << "  help                       Show this help\n";
}

std::pair<std::string, bool> dbus_call(const std::string& method,
                                        const std::string& args = "") {
    std::string cmd = "busctl call org.straylight.Mesh1 "
                      "/org/straylight/Mesh1 org.straylight.Mesh1 "
                      + method;
    if (!args.empty()) {
        cmd += " " + args;
    }
    cmd += " 2>/dev/null";

    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return {"", false};

    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) {
        result += buf;
    }
    int status = pclose(pipe);
    return {result, status == 0};
}

/// Parse a VRAM size string like "8G", "512M", "1T" into bytes.
size_t parse_vram_size(const std::string& s) {
    if (s.empty()) return 0;

    char suffix = s.back();
    double val = std::strtod(s.c_str(), nullptr);

    switch (suffix) {
        case 'T': case 't': return static_cast<size_t>(val * 1024.0 * 1024.0 * 1024.0 * 1024.0);
        case 'G': case 'g': return static_cast<size_t>(val * 1024.0 * 1024.0 * 1024.0);
        case 'M': case 'm': return static_cast<size_t>(val * 1024.0 * 1024.0);
        case 'K': case 'k': return static_cast<size_t>(val * 1024.0);
        default:            return static_cast<size_t>(val);
    }
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

int cmd_pool() {
    auto [output, ok] = dbus_call("ListGpus");

    if (!ok || output.empty()) {
        std::cout << "HOST             GPU  NAME                 VENDOR     VRAM(GiB)  FREE(GiB)  TEMP  UTIL  LATENCY\n";
        std::cout << "---------------  ---  -------------------  ---------  ---------  ---------  ----  ----  -------\n";
        std::cout << "(no D-Bus connection -- daemon may not be running)\n";
        std::cout << "\nTip: Start the mesh daemon with: systemctl start straylight-mesh\n";
        return 1;
    }

    std::cout << output;
    return 0;
}

int cmd_status() {
    auto [output, ok] = dbus_call("PoolStatus");

    if (!ok || output.empty()) {
        std::cout << "StrayLight Mesh Status\n";
        std::cout << "======================\n";
        std::cout << "Daemon:    not reachable (D-Bus unavailable)\n";
        std::cout << "Nodes:     unknown\n";
        std::cout << "GPUs:      unknown\n";
        std::cout << "\nTip: Start the mesh daemon with: systemctl start straylight-mesh\n";
        return 1;
    }

    std::cout << output;
    return 0;
}

int cmd_run(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Error: 'run' requires a command to execute\n";
        std::cerr << "Usage: straylight-mesh run <command> [--vram 8G]\n";
        return 1;
    }

    std::string command;
    size_t vram_needed = 0;

    for (int i = 2; i < argc; i++) {
        if (std::string(argv[i]) == "--vram" && i + 1 < argc) {
            vram_needed = parse_vram_size(argv[i + 1]);
            i++; // skip value
            continue;
        }
        if (!command.empty()) command += " ";
        command += argv[i];
    }

    std::cout << "Submitting: " << command << "\n";
    if (vram_needed > 0) {
        std::cout << "VRAM required: " << (vram_needed / (1024 * 1024 * 1024)) << " GiB\n";
    }

    auto [output, ok] = dbus_call("Submit",
        "st \"" + command + "\" " + std::to_string(vram_needed));

    if (!ok || output.empty()) {
        std::cerr << "Error: failed to submit (daemon not reachable)\n";
        return 1;
    }

    if (output.substr(0, 6) == "ERROR:") {
        std::cerr << output << "\n";
        return 1;
    }

    std::cout << "Output:\n" << output;
    return 0;
}

int cmd_transfer(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "Error: 'transfer' requires source and destination\n";
        std::cerr << "Usage: straylight-mesh transfer <host:gpu:handle> <host:gpu:handle>\n";
        std::cerr << "Example: straylight-mesh transfer localhost:0:42 198.51.100.5:1:17\n";
        return 1;
    }

    // Parse source: host:gpu:handle
    auto parse_spec = [](const std::string& spec) -> std::tuple<std::string, uint32_t, uint64_t, bool> {
        auto pos1 = spec.find(':');
        auto pos2 = spec.find(':', pos1 + 1);
        if (pos1 == std::string::npos || pos2 == std::string::npos) {
            return {"", 0, 0, false};
        }
        std::string host = spec.substr(0, pos1);
        uint32_t gpu = static_cast<uint32_t>(std::stoul(spec.substr(pos1 + 1, pos2 - pos1 - 1)));
        uint64_t handle = std::stoull(spec.substr(pos2 + 1));
        return {host, gpu, handle, true};
    };

    auto [src_host, src_gpu, src_handle, src_ok] = parse_spec(argv[2]);
    auto [dst_host, dst_gpu, dst_handle, dst_ok] = parse_spec(argv[3]);

    if (!src_ok || !dst_ok) {
        std::cerr << "Error: invalid spec format. Use host:gpu:handle\n";
        return 1;
    }

    std::cout << "Transfer: " << src_host << ":gpu" << src_gpu << ":handle" << src_handle
              << " -> " << dst_host << ":gpu" << dst_gpu << ":handle" << dst_handle << "\n";

    auto [output, ok] = dbus_call("Transfer",
        "sutsut " +
        src_host + " " + std::to_string(src_gpu) + " " + std::to_string(src_handle) + " " +
        dst_host + " " + std::to_string(dst_gpu) + " " + std::to_string(dst_handle) + " 0");

    if (!ok || output.empty()) {
        std::cerr << "Error: transfer failed (daemon not reachable)\n";
        return 1;
    }

    std::cout << "Result: " << output;
    return 0;
}

int cmd_benchmark() {
    std::cout << "StrayLight Mesh Interconnect Benchmark\n";
    std::cout << "======================================\n\n";

    // First, get the pool
    auto [pool_output, pool_ok] = dbus_call("ListGpus");

    if (!pool_ok || pool_output.empty()) {
        std::cout << "Cannot connect to mesh daemon. Running local-only benchmark.\n\n";
    } else {
        std::cout << "Connected GPUs:\n" << pool_output << "\n";
    }

    // Benchmark: allocate, transfer, free across pairs
    std::cout << "Running transfer benchmarks...\n\n";

    // Local memory bandwidth test
    {
        const size_t test_size = 256 * 1024 * 1024; // 256 MiB
        std::vector<uint8_t> src(test_size, 0xAB);
        std::vector<uint8_t> dst(test_size, 0);

        auto start = std::chrono::high_resolution_clock::now();
        std::memcpy(dst.data(), src.data(), test_size);
        auto end = std::chrono::high_resolution_clock::now();

        double elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();
        double gbps = (static_cast<double>(test_size) / (1024.0 * 1024.0 * 1024.0)) /
                       (elapsed_ms / 1000.0);

        std::cout << "  Local memcpy (256 MiB):     " << std::fixed << std::setprecision(1)
                  << elapsed_ms << " ms  (" << gbps << " GiB/s)\n";
    }

    // P2P DMA test (via daemon)
    auto [alloc_output, alloc_ok] = dbus_call("Allocate",
        "ts " + std::to_string(1024 * 1024) + " \"least_loaded\"");
    if (alloc_ok && !alloc_output.empty() && alloc_output.substr(0, 6) != "ERROR:") {
        std::cout << "  Mesh allocation (1 MiB):    OK (" << alloc_output << ")\n";
    } else {
        std::cout << "  Mesh allocation:            skipped (daemon unavailable)\n";
    }

    std::cout << "\nBenchmark complete.\n";
    return 0;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string cmd = argv[1];

    if (cmd == "pool")        return cmd_pool();
    if (cmd == "status")      return cmd_status();
    if (cmd == "run")         return cmd_run(argc, argv);
    if (cmd == "transfer")    return cmd_transfer(argc, argv);
    if (cmd == "benchmark")   return cmd_benchmark();
    if (cmd == "help" || cmd == "--help" || cmd == "-h") {
        print_usage();
        return 0;
    }

    std::cerr << "Error: unknown command '" << cmd << "'\n\n";
    print_usage();
    return 1;
}
