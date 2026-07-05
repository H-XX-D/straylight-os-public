// tools/sandbox/main.cpp
// CLI front-end for straylight-sandbox — lightweight isolated environments.

#include "sandbox_manager.h"

#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

static void print_usage() {
    std::cerr
        << "straylight-sandbox — lightweight isolated environments\n"
        << "\n"
        << "Usage:\n"
        << "  straylight-sandbox create <name> [--gpu] [--network] [--memory 4G] [--bind /path]\n"
        << "  straylight-sandbox enter <name>\n"
        << "  straylight-sandbox exec <name> <command...>\n"
        << "  straylight-sandbox list\n"
        << "  straylight-sandbox destroy <name>\n"
        << "  straylight-sandbox snapshot <name> <snap-name>\n"
        << "  straylight-sandbox export <name> <path.tar.gz>\n";
}

/// Parse a memory string like "4G", "512M", "2048" into megabytes.
static size_t parse_memory(const std::string& s) {
    if (s.empty()) return 4096;
    size_t val = static_cast<size_t>(std::atoll(s.c_str()));
    char suffix = s.back();
    if (suffix == 'G' || suffix == 'g') return val * 1024;
    if (suffix == 'M' || suffix == 'm') return val;
    if (suffix == 'K' || suffix == 'k') return val / 1024;
    // No suffix — assume megabytes.
    return val;
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

    straylight::SandboxManager mgr;

    // -------------------------------------------------------------------
    // create
    // -------------------------------------------------------------------
    if (command == "create") {
        if (argc < 3) {
            std::cerr << "Error: 'create' requires a sandbox name\n";
            return 1;
        }

        straylight::SandboxConfig config;
        config.name = argv[2];

        for (int i = 3; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--gpu") {
                config.gpu_passthrough = true;
            } else if (arg == "--network") {
                config.network = true;
            } else if (arg == "--no-network") {
                config.network = false;
            } else if (arg == "--memory" && i + 1 < argc) {
                config.memory_limit_mb = parse_memory(argv[++i]);
            } else if (arg == "--cpu-shares" && i + 1 < argc) {
                config.cpu_shares = static_cast<size_t>(std::atoi(argv[++i]));
            } else if (arg == "--base" && i + 1 < argc) {
                config.base_image = argv[++i];
            } else if (arg == "--bind" && i + 1 < argc) {
                config.bind_mounts.push_back(argv[++i]);
            }
        }

        auto res = mgr.create(config);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Sandbox '" << config.name << "' created.\n";
        std::cout << "  Memory limit: " << config.memory_limit_mb << " MiB\n";
        std::cout << "  GPU:          " << (config.gpu_passthrough ? "yes" : "no") << "\n";
        std::cout << "  Network:      " << (config.network ? "yes" : "no") << "\n";
        std::cout << "\nUse 'straylight-sandbox enter " << config.name
                  << "' to start a shell.\n";
        return 0;
    }

    // -------------------------------------------------------------------
    // enter
    // -------------------------------------------------------------------
    if (command == "enter") {
        if (argc < 3) {
            std::cerr << "Error: 'enter' requires a sandbox name\n";
            return 1;
        }
        auto res = mgr.enter(argv[2]);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        return 0;
    }

    // -------------------------------------------------------------------
    // exec
    // -------------------------------------------------------------------
    if (command == "exec") {
        if (argc < 4) {
            std::cerr << "Error: 'exec' requires a sandbox name and command\n";
            return 1;
        }
        // Join remaining args into a single command string.
        std::string cmd;
        for (int i = 3; i < argc; ++i) {
            if (i > 3) cmd += " ";
            cmd += argv[i];
        }
        auto res = mgr.run_in(argv[2], cmd);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << res.value();
        return 0;
    }

    // -------------------------------------------------------------------
    // list
    // -------------------------------------------------------------------
    if (command == "list") {
        auto sandboxes = mgr.list();
        if (sandboxes.empty()) {
            std::cout << "No sandboxes found.\n";
            return 0;
        }
        std::cout << std::left
                  << std::setw(24) << "NAME"
                  << std::setw(10) << "STATE"
                  << std::setw(10) << "PID"
                  << std::setw(12) << "MEMORY"
                  << std::setw(6) << "GPU"
                  << std::setw(6) << "NET"
                  << "\n";
        std::cout << std::string(68, '-') << "\n";
        for (const auto& sb : sandboxes) {
            std::string mem_str = std::to_string(sb.memory_limit_mb) + " MiB";
            std::string pid_str = sb.pid > 0 ? std::to_string(sb.pid) : "-";
            std::cout << std::left
                      << std::setw(24) << sb.name
                      << std::setw(10) << sb.state
                      << std::setw(10) << pid_str
                      << std::setw(12) << mem_str
                      << std::setw(6) << (sb.gpu ? "yes" : "no")
                      << std::setw(6) << (sb.network ? "yes" : "no")
                      << "\n";
        }
        return 0;
    }

    // -------------------------------------------------------------------
    // destroy
    // -------------------------------------------------------------------
    if (command == "destroy") {
        if (argc < 3) {
            std::cerr << "Error: 'destroy' requires a sandbox name\n";
            return 1;
        }
        auto res = mgr.destroy(argv[2]);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Sandbox '" << argv[2] << "' destroyed.\n";
        return 0;
    }

    // -------------------------------------------------------------------
    // snapshot
    // -------------------------------------------------------------------
    if (command == "snapshot") {
        if (argc < 4) {
            std::cerr << "Error: 'snapshot' requires a sandbox name and snapshot name\n";
            return 1;
        }
        auto res = mgr.snapshot(argv[2], argv[3]);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Snapshot '" << argv[3] << "' created for sandbox '"
                  << argv[2] << "'.\n";
        return 0;
    }

    // -------------------------------------------------------------------
    // export
    // -------------------------------------------------------------------
    if (command == "export") {
        if (argc < 4) {
            std::cerr << "Error: 'export' requires a sandbox name and output path\n";
            return 1;
        }
        auto res = mgr.export_tar(argv[2], argv[3]);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Sandbox '" << argv[2] << "' exported to " << argv[3] << "\n";
        return 0;
    }

    std::cerr << "Error: unknown command '" << command << "'\n";
    print_usage();
    return 1;
}
