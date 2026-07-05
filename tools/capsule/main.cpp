// tools/capsule/main.cpp
// straylight-capsule — resource-contract app packages for StrayLight OS.
// Build, install, run, and manage capsule applications.

#include "capsule_builder.h"
#include "capsule_installer.h"
#include "capsule_manifest.h"
#include "capsule_runner.h"

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

static void print_usage() {
    std::cerr
        << "straylight-capsule — resource-contract app packages\n"
        << "\n"
        << "Usage:\n"
        << "  straylight-capsule build <directory> [--output <path>]  Build .capsule from app dir\n"
        << "  straylight-capsule install <capsule-file>               Install a .capsule package\n"
        << "  straylight-capsule uninstall <name>                     Uninstall a capsule\n"
        << "  straylight-capsule run <name> [-- args...]              Run an installed capsule\n"
        << "  straylight-capsule stop <name>                          Stop a running capsule\n"
        << "  straylight-capsule list                                 List installed capsules\n"
        << "  straylight-capsule running                              List running capsules\n"
        << "  straylight-capsule info <name>                          Show capsule details\n"
        << "  straylight-capsule verify <capsule-file>                Verify package integrity\n";
}

static int cmd_build(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "Error: build requires a directory argument\n";
        return 1;
    }

    std::string output_path;
    std::string directory = args[0];

    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--output" && i + 1 < args.size()) {
            output_path = args[++i];
        }
    }

    straylight::Result<std::string, std::string> result =
        output_path.empty()
            ? straylight::CapsuleBuilder::build(directory)
            : straylight::CapsuleBuilder::build(directory, output_path);

    if (!result.has_value()) {
        std::cerr << "Error: " << result.error() << "\n";
        return 1;
    }

    std::cout << "Built: " << result.value() << "\n";
    return 0;
}

static int cmd_install(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "Error: install requires a capsule file argument\n";
        return 1;
    }

    auto result = straylight::CapsuleInstaller::install(args[0]);
    if (!result.has_value()) {
        std::cerr << "Error: " << result.error() << "\n";
        return 1;
    }

    return 0;
}

static int cmd_uninstall(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "Error: uninstall requires a capsule name\n";
        return 1;
    }

    auto result = straylight::CapsuleInstaller::uninstall(args[0]);
    if (!result.has_value()) {
        std::cerr << "Error: " << result.error() << "\n";
        return 1;
    }

    return 0;
}

static int cmd_run(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "Error: run requires a capsule name\n";
        return 1;
    }

    std::string name = args[0];
    std::vector<std::string> capsule_args;

    // Collect args after "--"
    bool after_separator = false;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--") {
            after_separator = true;
            continue;
        }
        if (after_separator) {
            capsule_args.push_back(args[i]);
        }
    }

    auto result = straylight::CapsuleRunner::run(name, capsule_args);
    if (!result.has_value()) {
        std::cerr << "Error: " << result.error() << "\n";
        return 1;
    }

    return 0;
}

static int cmd_stop(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "Error: stop requires a capsule name\n";
        return 1;
    }

    auto result = straylight::CapsuleRunner::stop(args[0]);
    if (!result.has_value()) {
        std::cerr << "Error: " << result.error() << "\n";
        return 1;
    }

    return 0;
}

static int cmd_list([[maybe_unused]] const std::vector<std::string>& args) {
    auto capsules = straylight::CapsuleInstaller::list_installed();

    if (capsules.empty()) {
        std::cout << "No capsules installed.\n";
        return 0;
    }

    std::cout << std::left
              << std::setw(24) << "NAME"
              << std::setw(12) << "VERSION"
              << std::setw(10) << "RAM"
              << std::setw(10) << "VRAM"
              << std::setw(8)  << "GPU%"
              << std::setw(8)  << "CORES"
              << "DESCRIPTION"
              << "\n";

    std::cout << std::string(90, '-') << "\n";

    for (const auto& c : capsules) {
        std::cout << std::left
                  << std::setw(24) << c.name
                  << std::setw(12) << c.version
                  << std::setw(10) << (std::to_string(c.resource_contract.min_ram_mb) + "MB")
                  << std::setw(10) << (std::to_string(c.resource_contract.min_vram_mb) + "MB")
                  << std::setw(8)  << (std::to_string(c.resource_contract.gpu_compute_percent) + "%")
                  << std::setw(8)  << c.resource_contract.min_cpu_cores
                  << c.description
                  << "\n";
    }

    return 0;
}

static int cmd_running([[maybe_unused]] const std::vector<std::string>& args) {
    auto running = straylight::CapsuleRunner::list_running();

    if (running.empty()) {
        std::cout << "No capsules running.\n";
        return 0;
    }

    std::cout << std::left
              << std::setw(24) << "NAME"
              << std::setw(8)  << "PID"
              << std::setw(14) << "RAM(use/cap)"
              << std::setw(14) << "VRAM(use/cap)"
              << std::setw(10) << "CPU%"
              << std::setw(12) << "UPTIME"
              << "\n";

    std::cout << std::string(82, '-') << "\n";

    for (const auto& c : running) {
        std::string ram_str = std::to_string(c.current_ram_mb) + "/" +
                              std::to_string(c.contract.min_ram_mb) + "MB";
        std::string vram_str = std::to_string(c.current_vram_mb) + "/" +
                               std::to_string(c.contract.min_vram_mb) + "MB";

        // Format uptime
        std::string uptime;
        if (c.uptime_seconds >= 86400) {
            uptime = std::to_string(c.uptime_seconds / 86400) + "d " +
                     std::to_string((c.uptime_seconds % 86400) / 3600) + "h";
        } else if (c.uptime_seconds >= 3600) {
            uptime = std::to_string(c.uptime_seconds / 3600) + "h " +
                     std::to_string((c.uptime_seconds % 3600) / 60) + "m";
        } else {
            uptime = std::to_string(c.uptime_seconds / 60) + "m " +
                     std::to_string(c.uptime_seconds % 60) + "s";
        }

        // Warn if exceeding contract
        bool over_ram = c.current_ram_mb > c.contract.min_ram_mb && c.contract.min_ram_mb > 0;

        std::cout << std::left
                  << std::setw(24) << c.name
                  << std::setw(8)  << c.pid
                  << std::setw(14) << (over_ram ? ("!" + ram_str) : ram_str)
                  << std::setw(14) << vram_str
                  << std::setw(10) << (std::to_string(static_cast<int>(c.cpu_usage_percent)) + "%")
                  << std::setw(12) << uptime
                  << "\n";
    }

    return 0;
}

static int cmd_info(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "Error: info requires a capsule name\n";
        return 1;
    }

    auto result = straylight::CapsuleInstaller::get_installed(args[0]);
    if (!result.has_value()) {
        std::cerr << "Error: " << result.error() << "\n";
        return 1;
    }

    const auto& c = result.value();
    const auto& rc = c.resource_contract;

    std::cout << "Name:        " << c.name << "\n"
              << "Version:     " << c.version << "\n"
              << "Description: " << c.description << "\n"
              << "Install:     " << c.install_path << "\n"
              << "\n"
              << "Resource Contract:\n"
              << "  RAM:         " << rc.min_ram_mb << " MB minimum\n"
              << "  VRAM:        " << rc.min_vram_mb << " MB minimum\n"
              << "  GPU compute: " << rc.gpu_compute_percent << "%\n"
              << "  CPU cores:   " << rc.min_cpu_cores << " minimum\n"
              << "  Disk:        " << rc.max_disk_mb << " MB maximum\n"
              << "  Mesh:        " << (rc.requires_mesh ? "required" : "not required") << "\n"
              << "  Network:     " << (rc.requires_network ? "required" : "not required") << "\n";

    return 0;
}

static int cmd_verify(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "Error: verify requires a capsule file argument\n";
        return 1;
    }

    // Load manifest from the archive by extracting just capsule.json
    std::string cmd = "tar --zstd -xf '" + args[0] + "' --to-stdout '*/capsule.json' 2>/dev/null || "
                      "zstd -d -c '" + args[0] + "' | tar -xf - --to-stdout '*/capsule.json' 2>/dev/null";

    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) {
        std::cerr << "Error: cannot read archive\n";
        return 1;
    }

    std::string content;
    char buf[4096];
    while (::fgets(buf, sizeof(buf), pipe)) {
        content += buf;
    }
    int status = ::pclose(pipe);

    if (status != 0 || content.empty()) {
        std::cerr << "Error: cannot extract capsule.json from archive\n";
        return 1;
    }

    auto result = straylight::CapsuleManifestParser::parse(content);
    if (!result.has_value()) {
        std::cerr << "Error: invalid manifest: " << result.error() << "\n";
        return 1;
    }

    const auto& m = result.value();
    std::cout << "Valid capsule: " << m.name << " v" << m.version << "\n";
    std::cout << "Binary: " << m.binary_path << "\n";
    std::cout << "Dependencies: " << m.dependencies.size() << "\n";
    std::cout << "Contract: RAM≥" << m.resource_contract.min_ram_mb
              << "MB VRAM≥" << m.resource_contract.min_vram_mb
              << "MB GPU≤" << m.resource_contract.gpu_compute_percent
              << "% CPU≥" << m.resource_contract.min_cpu_cores << " cores\n";

    return 0;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string command = argv[1];
    std::vector<std::string> args;
    for (int i = 2; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }

    if (command == "build")     return cmd_build(args);
    if (command == "install")   return cmd_install(args);
    if (command == "uninstall") return cmd_uninstall(args);
    if (command == "run")       return cmd_run(args);
    if (command == "stop")      return cmd_stop(args);
    if (command == "list")      return cmd_list(args);
    if (command == "running")   return cmd_running(args);
    if (command == "info")      return cmd_info(args);
    if (command == "verify")    return cmd_verify(args);

    if (command == "--help" || command == "-h") {
        print_usage();
        return 0;
    }

    std::cerr << "Unknown command: " << command << "\n";
    print_usage();
    return 1;
}
