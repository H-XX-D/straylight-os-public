// tools/trace/main.cpp
// straylight-trace — Application performance tracer.
// Traces syscalls, file I/O, network, and memory for any process.

#include "tracer.h"
#include "report.h"

#include <straylight/log.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

static void print_usage() {
    std::cerr
        << "Usage:\n"
        << "  straylight-trace run <command> [args...]                  Trace from start to exit\n"
        << "  straylight-trace attach <pid>                             Attach to running process\n"
        << "  straylight-trace record <command> [args...] -o trace.json Record for later analysis\n"
        << "  straylight-trace report <trace.json>                      Report from recorded trace\n"
        << "  straylight-trace flamegraph <trace.json> -o output.folded Export flamegraph data\n"
        << "  straylight-trace chrome <trace.json> -o output.json       Export chrome://tracing\n"
        << "\n"
        << "Options:\n"
        << "  -o, --output <path>    Output file path\n"
        << "  --max-events <N>       Limit recorded events (default: unlimited)\n"
        << "  --no-events            Don't record individual events (summary only)\n"
        << "  --live                 Print syscalls live as they happen\n"
        << "  -h, --help             Show this help\n";
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

    straylight::Log::init("straylight-trace", straylight::Log::Level::Warn);

    if (command == "run" || command == "record") {
        // Parse options
        std::vector<std::string> cmd_argv;
        std::string output_path;
        size_t max_events = 0;
        bool no_events = false;
        bool live = false;

        bool passthrough = false;
        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];

            if (!passthrough && arg == "--") {
                passthrough = true;
                continue;
            }

            if (!passthrough && (arg == "-o" || arg == "--output")) {
                if (i + 1 >= argc) {
                    std::cerr << "Error: " << arg << " requires a path\n";
                    return 1;
                }
                output_path = argv[++i];
            } else if (!passthrough && arg == "--max-events") {
                if (i + 1 >= argc) {
                    std::cerr << "Error: --max-events requires a count\n";
                    return 1;
                }
                max_events = static_cast<size_t>(std::atol(argv[++i]));
            } else if (!passthrough && arg == "--no-events") {
                no_events = true;
            } else if (!passthrough && arg == "--live") {
                live = true;
            } else {
                cmd_argv.emplace_back(arg);
            }
        }

        if (cmd_argv.empty()) {
            std::cerr << "Error: no command specified\n";
            print_usage();
            return 1;
        }

        if (command == "record" && output_path.empty()) {
            output_path = "trace.json";
        }

        straylight::Tracer tracer;
        tracer.set_record_events(!no_events);
        if (max_events > 0) tracer.set_max_events(max_events);

        if (live) {
            tracer.set_callback([](const straylight::SyscallEvent& ev) {
                char dur_buf[32];
                if (ev.duration_ns < 1000) {
                    std::snprintf(dur_buf, sizeof(dur_buf), "%luns", static_cast<unsigned long>(ev.duration_ns));
                } else if (ev.duration_ns < 1000000) {
                    std::snprintf(dur_buf, sizeof(dur_buf), "%.1fus", static_cast<double>(ev.duration_ns) / 1e3);
                } else {
                    std::snprintf(dur_buf, sizeof(dur_buf), "%.2fms", static_cast<double>(ev.duration_ns) / 1e6);
                }

                std::cerr << "[" << ev.pid << "] "
                          << ev.syscall_name << "("
                          << ") = " << ev.return_value
                          << " [" << dur_buf << "]\n";
            });
        }

        std::cerr << "Tracing: ";
        for (const auto& a : cmd_argv) std::cerr << a << " ";
        std::cerr << "\n";

        auto result = tracer.run(cmd_argv);
        if (!result.has_value()) {
            std::cerr << "Error: " << result.error().message() << "\n";
            return 1;
        }

        const auto& data = result.value();

        // Print summary
        straylight::TraceReport::print_summary(data);

        // Export if output requested
        if (!output_path.empty()) {
            auto export_result = straylight::TraceReport::export_json(data, output_path);
            if (export_result.has_value()) {
                std::cerr << "Trace saved to: " << output_path << "\n";
            } else {
                std::cerr << "Error saving trace: " << export_result.error().message() << "\n";
            }
        }

        return data.exit_code;

    } else if (command == "attach") {
        if (argc < 3) {
            std::cerr << "Error: 'attach' requires a PID\n";
            return 1;
        }

        pid_t pid = static_cast<pid_t>(std::atoi(argv[2]));
        if (pid <= 0) {
            std::cerr << "Error: invalid PID '" << argv[2] << "'\n";
            return 1;
        }

        std::string output_path;
        bool live = false;

        for (int j = 3; j < argc; ++j) {
            std::string arg = argv[j];
            if ((arg == "-o" || arg == "--output") && j + 1 < argc) {
                output_path = argv[++j];
            } else if (arg == "--live") {
                live = true;
            }
        }

        straylight::Tracer tracer;

        if (live) {
            tracer.set_callback([](const straylight::SyscallEvent& ev) {
                std::cerr << "[" << ev.pid << "] " << ev.syscall_name
                          << " = " << ev.return_value << "\n";
            });
        }

        std::cerr << "Attaching to PID " << pid << " (Ctrl+C to stop)...\n";

        auto result = tracer.attach(pid);
        if (!result.has_value()) {
            std::cerr << "Error: " << result.error().message() << "\n";
            return 1;
        }

        straylight::TraceReport::print_summary(result.value());

        if (!output_path.empty()) {
            auto r = straylight::TraceReport::export_json(result.value(), output_path);
            if (r.has_value()) {
                std::cerr << "Trace saved to: " << output_path << "\n";
            }
        }

        return 0;

    } else if (command == "report") {
        if (argc < 3) {
            std::cerr << "Error: 'report' requires a trace file\n";
            return 1;
        }

        auto result = straylight::TraceReport::import_json(argv[2]);
        if (!result.has_value()) {
            std::cerr << "Error: " << result.error().message() << "\n";
            return 1;
        }

        straylight::TraceReport::print_summary(result.value());
        return 0;

    } else if (command == "flamegraph") {
        if (argc < 3) {
            std::cerr << "Error: 'flamegraph' requires a trace file\n";
            return 1;
        }

        std::string output_path;
        for (int j = 3; j < argc; ++j) {
            std::string arg = argv[j];
            if ((arg == "-o" || arg == "--output") && j + 1 < argc) {
                output_path = argv[++j];
            }
        }
        if (output_path.empty()) output_path = "trace.folded";

        auto data = straylight::TraceReport::import_json(argv[2]);
        if (!data.has_value()) {
            std::cerr << "Error: " << data.error().message() << "\n";
            return 1;
        }

        auto r = straylight::TraceReport::export_flamegraph(data.value(), output_path);
        if (r.has_value()) {
            std::cout << "Flamegraph data saved to: " << output_path << "\n";
            std::cout << "Generate SVG with: flamegraph.pl " << output_path << " > trace.svg\n";
        } else {
            std::cerr << "Error: " << r.error().message() << "\n";
            return 1;
        }
        return 0;

    } else if (command == "chrome") {
        if (argc < 3) {
            std::cerr << "Error: 'chrome' requires a trace file\n";
            return 1;
        }

        std::string output_path;
        for (int j = 3; j < argc; ++j) {
            std::string arg = argv[j];
            if ((arg == "-o" || arg == "--output") && j + 1 < argc) {
                output_path = argv[++j];
            }
        }
        if (output_path.empty()) output_path = "trace-chrome.json";

        auto data = straylight::TraceReport::import_json(argv[2]);
        if (!data.has_value()) {
            std::cerr << "Error: " << data.error().message() << "\n";
            return 1;
        }

        auto r = straylight::TraceReport::export_chrome_trace(data.value(), output_path);
        if (r.has_value()) {
            std::cout << "Chrome trace saved to: " << output_path << "\n";
            std::cout << "Open chrome://tracing and load the file to view.\n";
        } else {
            std::cerr << "Error: " << r.error().message() << "\n";
            return 1;
        }
        return 0;

    } else {
        std::cerr << "Error: unknown command '" << command << "'\n";
        print_usage();
        return 1;
    }
}
