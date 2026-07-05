// tools/bench/main.cpp
// StrayLight integrated hardware benchmarking tool.
// Measures real workloads: CPU, memory, storage, GPU, network, ML.

#include "benchmarks.h"
#include "report.h"

#include <straylight/log.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

static void print_usage() {
    std::cerr
        << "Usage:\n"
        << "  straylight-bench all                          Run full benchmark suite\n"
        << "  straylight-bench cpu [--threads N]             CPU compute benchmarks\n"
        << "  straylight-bench memory                        Memory bandwidth & latency\n"
        << "  straylight-bench gpu [--device N]              GPU compute & memory\n"
        << "  straylight-bench storage [--path /tmp]         Storage performance\n"
        << "  straylight-bench network [--target host:port]  Network throughput & latency\n"
        << "  straylight-bench ml [--model llama-7b]         ML inference benchmarks\n"
        << "  straylight-bench p2p                           GPU-to-GPU P2P bandwidth\n"
        << "  straylight-bench compare <previous.json>       Compare with previous run\n"
        << "  straylight-bench report                        Generate HTML report\n"
        << "\n"
        << "Options:\n"
        << "  --json <file>       Save results as JSON\n"
        << "  --html <file>       Save results as HTML report\n"
        << "  --output-dir <dir>  Save results to directory (default: ~/.config/straylight/benchmarks/)\n"
        << "  -q, --quiet         Suppress progress output\n";
}

static std::string results_dir() {
    const char* home = std::getenv("HOME");
    if (!home) home = "/tmp";
    std::string dir = std::string(home) + "/.config/straylight/benchmarks";
    std::filesystem::create_directories(dir);
    return dir;
}

static void print_results(const std::vector<straylight::BenchResult>& results) {
    std::string last_cat;
    for (const auto& r : results) {
        if (r.category != last_cat) {
            if (!last_cat.empty()) std::cout << "\n";
            std::string header = r.category;
            for (auto& c : header) c = static_cast<char>(std::toupper(c));
            std::cout << "=== " << header << " ===\n";
            last_cat = r.category;
        }
        std::cout << "  " << r.format() << "\n";
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string command = argv[1];

    if (command == "--help" || command == "-h") {
        print_usage();
        return 0;
    }

    // Parse global options
    std::string json_output;
    std::string html_output;
    std::string output_dir = results_dir();
    bool quiet = false;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--json" && i + 1 < argc) {
            json_output = argv[++i];
        } else if (arg == "--html" && i + 1 < argc) {
            html_output = argv[++i];
        } else if (arg == "--output-dir" && i + 1 < argc) {
            output_dir = argv[++i];
        } else if (arg == "-q" || arg == "--quiet") {
            quiet = true;
        }
    }

    if (!quiet) {
        straylight::Log::init("straylight-bench");
    }

    straylight::BenchmarkSuite suite;

    if (command == "all") {
        // Parse options
        std::string storage_path = "/tmp";
        std::string network_target;

        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--path" && i + 1 < argc) {
                storage_path = argv[++i];
            } else if (arg == "--target" && i + 1 < argc) {
                network_target = argv[++i];
            }
        }

        if (!quiet) std::cout << "Running full benchmark suite...\n\n";

        auto report = suite.run_all(storage_path, network_target);
        print_results(report.results);

        // Auto-save JSON
        std::string auto_json = output_dir + "/" + report.timestamp + ".json";
        // Replace colons in filename
        for (auto& c : auto_json) {
            if (c == ':') c = '-';
        }
        auto save_result = straylight::ReportGenerator::save_json(report, auto_json);
        if (save_result.has_value()) {
            std::cout << "\nResults saved to: " << auto_json << "\n";
        }

        // Save explicit outputs
        if (!json_output.empty()) {
            straylight::ReportGenerator::save_json(report, json_output);
            std::cout << "JSON saved to: " << json_output << "\n";
        }
        if (!html_output.empty()) {
            auto html_result = straylight::ReportGenerator::generate_html(report);
            if (html_result.has_value()) {
                std::ofstream f(html_output);
                f << html_result.value();
                std::cout << "HTML report saved to: " << html_output << "\n";
            }
        }

    } else if (command == "cpu") {
        int threads = 0;
        for (int i = 2; i < argc; ++i) {
            if (std::string(argv[i]) == "--threads" && i + 1 < argc) {
                threads = std::atoi(argv[++i]);
            }
        }

        if (!quiet) std::cout << "Running CPU benchmarks...\n\n";

        std::vector<straylight::BenchResult> results;
        results.push_back(suite.cpu_single_thread());
        results.push_back(suite.cpu_multi_thread(threads));
        results.push_back(suite.cpu_avx_bandwidth());
        results.push_back(suite.cpu_context_switch());
        print_results(results);

    } else if (command == "memory") {
        if (!quiet) std::cout << "Running memory benchmarks...\n\n";

        std::vector<straylight::BenchResult> results;
        results.push_back(suite.mem_sequential_read());
        results.push_back(suite.mem_sequential_write());
        results.push_back(suite.mem_random_access());
        results.push_back(suite.mem_numa_bandwidth());
        print_results(results);

    } else if (command == "gpu") {
        int device = 0;
        for (int i = 2; i < argc; ++i) {
            if (std::string(argv[i]) == "--device" && i + 1 < argc) {
                device = std::atoi(argv[++i]);
            }
        }

        if (!quiet) std::cout << "Running GPU benchmarks...\n\n";

        std::vector<straylight::BenchResult> results;
        results.push_back(suite.gpu_memory_bandwidth(device));
        results.push_back(suite.gpu_compute_flops(device));
        print_results(results);

    } else if (command == "storage") {
        std::string path = "/tmp";
        for (int i = 2; i < argc; ++i) {
            if (std::string(argv[i]) == "--path" && i + 1 < argc) {
                path = argv[++i];
            }
        }

        if (!quiet) std::cout << "Running storage benchmarks on " << path << "...\n\n";

        std::vector<straylight::BenchResult> results;
        results.push_back(suite.storage_seq_read(path));
        results.push_back(suite.storage_seq_write(path));
        results.push_back(suite.storage_rand_read(path));
        results.push_back(suite.storage_rand_write(path));
        results.push_back(suite.storage_latency(path));
        print_results(results);

    } else if (command == "network") {
        std::string target;
        for (int i = 2; i < argc; ++i) {
            if (std::string(argv[i]) == "--target" && i + 1 < argc) {
                target = argv[++i];
            }
        }

        if (target.empty()) {
            std::cerr << "Error: --target required for network benchmarks\n";
            return 1;
        }

        if (!quiet) std::cout << "Running network benchmarks to " << target << "...\n\n";

        std::vector<straylight::BenchResult> results;
        results.push_back(suite.net_throughput(target));
        results.push_back(suite.net_latency(target));
        print_results(results);

    } else if (command == "ml") {
        if (!quiet) std::cout << "Running ML inference benchmark...\n\n";

        std::vector<straylight::BenchResult> results;
        results.push_back(suite.ml_inference_throughput());
        print_results(results);

    } else if (command == "p2p") {
        int dev_a = 0, dev_b = 1;
        for (int i = 2; i < argc; ++i) {
            if (std::string(argv[i]) == "--devices" && i + 2 < argc) {
                dev_a = std::atoi(argv[++i]);
                dev_b = std::atoi(argv[++i]);
            }
        }

        if (!quiet) std::cout << "Running GPU P2P bandwidth test...\n\n";

        auto result = suite.gpu_p2p_bandwidth(dev_a, dev_b);
        std::cout << "  " << result.format() << "\n";

    } else if (command == "compare") {
        if (argc < 3) {
            std::cerr << "Error: 'compare' requires a previous results JSON file\n";
            return 1;
        }

        std::string baseline_path = argv[2];
        auto baseline_result = straylight::ReportGenerator::load_json(baseline_path);
        if (!baseline_result.has_value()) {
            std::cerr << "Error loading baseline: " << baseline_result.error() << "\n";
            return 1;
        }

        if (!quiet) std::cout << "Running benchmarks for comparison...\n\n";

        std::string storage_path = "/tmp";
        std::string network_target;
        for (int i = 3; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--path" && i + 1 < argc) storage_path = argv[++i];
            if (arg == "--target" && i + 1 < argc) network_target = argv[++i];
        }

        auto current = suite.run_all(storage_path, network_target);
        std::string comparison = straylight::BenchmarkSuite::compare(
            baseline_result.value(), current);
        std::cout << comparison;

        // Generate comparison HTML if requested
        if (!html_output.empty()) {
            auto html_result = straylight::ReportGenerator::generate_comparison_html(
                baseline_result.value(), current);
            if (html_result.has_value()) {
                std::ofstream f(html_output);
                f << html_result.value();
                std::cout << "\nHTML comparison saved to: " << html_output << "\n";
            }
        }

    } else if (command == "report") {
        // Find most recent results JSON and generate HTML
        std::string dir = output_dir;
        std::string latest_json;
        std::filesystem::file_time_type latest_time{};

        if (std::filesystem::exists(dir)) {
            for (const auto& entry : std::filesystem::directory_iterator(dir)) {
                if (entry.path().extension() == ".json") {
                    auto mod_time = entry.last_write_time();
                    if (latest_json.empty() || mod_time > latest_time) {
                        latest_json = entry.path().string();
                        latest_time = mod_time;
                    }
                }
            }
        }

        if (latest_json.empty()) {
            std::cerr << "No benchmark results found. Run 'straylight-bench all' first.\n";
            return 1;
        }

        auto report_result = straylight::ReportGenerator::load_json(latest_json);
        if (!report_result.has_value()) {
            std::cerr << "Error loading results: " << report_result.error() << "\n";
            return 1;
        }

        std::string out_path = html_output.empty()
            ? (dir + "/report.html")
            : html_output;

        auto html_result = straylight::ReportGenerator::generate_html(
            report_result.value());
        if (!html_result.has_value()) {
            std::cerr << "Error generating report: " << html_result.error() << "\n";
            return 1;
        }

        std::ofstream f(out_path);
        if (!f.is_open()) {
            std::cerr << "Cannot write to " << out_path << "\n";
            return 1;
        }
        f << html_result.value();
        f.close();

        std::cout << "HTML report generated: " << out_path << "\n";

    } else {
        std::cerr << "Error: unknown command '" << command << "'\n";
        print_usage();
        return 1;
    }

    return 0;
}
