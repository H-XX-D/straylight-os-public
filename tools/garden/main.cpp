// tools/garden/main.cpp
// straylight-garden — Isolated development environment manager.
// Creates, manages, and activates per-project dependency environments.

#include "garden_manager.h"
#include "environment.h"

#include <straylight/log.h>

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

static void print_usage() {
    std::cerr
        << "Usage:\n"
        << "  straylight-garden init <name> [--desc DESC]      Create new environment\n"
        << "  straylight-garden enter <name>                    Enter environment (eval output)\n"
        << "  straylight-garden exit                            Exit current environment\n"
        << "  straylight-garden install <name> <pkg...>         Install packages\n"
        << "       [--method pip|cargo|apt|manual]\n"
        << "  straylight-garden list                            List all environments\n"
        << "  straylight-garden info <name>                     Show environment details\n"
        << "  straylight-garden export <name> <file.json>       Export environment spec\n"
        << "  straylight-garden import <file.json> [--name N]   Import environment spec\n"
        << "  straylight-garden clone <name> <new-name>         Clone environment\n"
        << "  straylight-garden destroy <name>                  Remove environment\n"
        << "  straylight-garden env <name>                      Print env vars for shell\n"
        << "\n"
        << "Shell integration:\n"
        << "  eval $(straylight-garden enter myenv)\n"
        << "  eval $(straylight-garden exit)\n"
        << "  garden-run <command>                              Run command under NUMA profile\n";
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

    straylight::GardenManager mgr;

    if (command == "init") {
        if (argc < 3) {
            std::cerr << "Error: 'init' requires an environment name\n";
            return 1;
        }

        std::string name = argv[2];
        std::string desc;
        for (int i = 3; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--desc" && i + 1 < argc) {
                desc = argv[++i];
            }
        }

        auto result = mgr.create(name, desc);
        if (!result.has_value()) {
            std::cerr << "Error: " << result.error().message() << "\n";
            return 1;
        }

        std::cout << "Created garden: " << name << "\n";
        std::cout << "Location: " << result.value().root_dir() << "\n";
        std::cout << "\nTo enter: eval $(straylight-garden enter " << name << ")\n";
        return 0;

    } else if (command == "enter") {
        if (argc < 3) {
            std::cerr << "Error: 'enter' requires an environment name\n";
            return 1;
        }

        auto env = mgr.load(argv[2]);
        if (!env.has_value()) {
            std::cerr << "Error: " << env.error().message() << "\n";
            return 1;
        }

        // Output the activation script to stdout — caller should eval it
        std::cout << env.value().generate_enter_script();
        return 0;

    } else if (command == "exit") {
        // Check if we're in a garden
        const char* garden_name = ::getenv("_GARDEN_NAME");
        if (!garden_name || garden_name[0] == '\0') {
            std::cerr << "Not in a garden environment\n";
            return 1;
        }

        auto env = mgr.load(garden_name);
        if (!env.has_value()) {
            // Even if we can't load the spec, try to restore basic state
            std::cout << "export PATH=\"$_GARDEN_OLD_PATH\"\n";
            std::cout << "export PS1=\"$_GARDEN_OLD_PS1\"\n";
            std::cout << "unset _GARDEN_OLD_PATH _GARDEN_OLD_PS1 _GARDEN_NAME _GARDEN_ROOT\n";
            return 0;
        }

        std::cout << env.value().generate_exit_script();
        return 0;

    } else if (command == "install") {
        if (argc < 4) {
            std::cerr << "Error: 'install' requires environment name and package name(s)\n";
            return 1;
        }

        std::string env_name = argv[2];
        std::string method = "manual";
        std::vector<std::string> packages;

        for (int i = 3; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--method" && i + 1 < argc) {
                method = argv[++i];
            } else {
                packages.push_back(arg);
            }
        }

        for (const auto& pkg : packages) {
            std::cout << "Installing " << pkg << " (method: " << method << ")...\n";
            auto r = mgr.install_package(env_name, pkg, method);
            if (!r.has_value()) {
                std::cerr << "Error installing " << pkg << ": " << r.error().message() << "\n";
            } else {
                std::cout << "  Installed: " << pkg << "\n";
            }
        }

        return 0;

    } else if (command == "list") {
        auto envs = mgr.list();

        if (envs.empty()) {
            std::cout << "No gardens found.\n";
            std::cout << "Create one: straylight-garden init <name>\n";
            return 0;
        }

        std::cout << std::left << std::setw(20) << "NAME"
                  << std::setw(30) << "DESCRIPTION"
                  << std::setw(8) << "PKGS"
                  << std::setw(8) << "PYTHON"
                  << std::setw(20) << "CREATED" << "\n";
        std::cout << std::string(86, '-') << "\n";

        for (const auto& env : envs) {
            std::string desc = env.description;
            if (desc.size() > 28) desc = desc.substr(0, 25) + "...";

            std::cout << std::left << std::setw(20) << env.name
                      << std::setw(30) << desc
                      << std::setw(8) << env.package_count
                      << std::setw(8) << (env.has_python_venv ? "yes" : "no")
                      << std::setw(20) << env.created_at << "\n";
        }

        return 0;

    } else if (command == "info") {
        if (argc < 3) {
            std::cerr << "Error: 'info' requires an environment name\n";
            return 1;
        }

        auto env = mgr.load(argv[2]);
        if (!env.has_value()) {
            std::cerr << "Error: " << env.error().message() << "\n";
            return 1;
        }

        const auto& spec = env.value().spec();
        std::cout << "Name:        " << spec.name << "\n";
        std::cout << "Description: " << spec.description << "\n";
        std::cout << "Created:     " << spec.created_at << "\n";
        std::cout << "Modified:    " << spec.modified_at << "\n";
        std::cout << "Root:        " << env.value().root_dir() << "\n";
        std::cout << "\n";

        if (!spec.env_vars.empty()) {
            std::cout << "Environment Variables:\n";
            for (const auto& ev : spec.env_vars) {
                std::cout << "  " << ev.key << "=" << ev.value;
                if (ev.append) std::cout << " (append)";
                std::cout << "\n";
            }
            std::cout << "\n";
        }

        if (!spec.path_prepend.empty()) {
            std::cout << "PATH prepend:\n";
            for (const auto& p : spec.path_prepend) {
                std::cout << "  " << p << "\n";
            }
            std::cout << "\n";
        }

        if (spec.python.enabled) {
            std::cout << "Python:\n";
            std::cout << "  Version: " << spec.python.python_version << "\n";
            if (!spec.python.pip_packages.empty()) {
                std::cout << "  Packages:\n";
                for (const auto& p : spec.python.pip_packages) {
                    std::cout << "    - " << p << "\n";
                }
            }
            std::cout << "\n";
        }

        if (!spec.packages.empty()) {
            std::cout << "Installed Packages:\n";
            for (const auto& pkg : spec.packages) {
                std::cout << "  " << pkg.name << " (" << pkg.install_method << ")";
                if (!pkg.version.empty()) std::cout << " v" << pkg.version;
                std::cout << "\n";
            }
            std::cout << "\n";
        }

        if (!spec.hooks.on_enter.empty()) {
            std::cout << "On-enter hook:\n  " << spec.hooks.on_enter << "\n\n";
        }
        if (!spec.hooks.on_exit.empty()) {
            std::cout << "On-exit hook:\n  " << spec.hooks.on_exit << "\n\n";
        }

        return 0;

    } else if (command == "export") {
        if (argc < 4) {
            std::cerr << "Error: 'export' requires environment name and output file\n";
            return 1;
        }

        auto env = mgr.load(argv[2]);
        if (!env.has_value()) {
            std::cerr << "Error: " << env.error().message() << "\n";
            return 1;
        }

        auto r = env.value().export_spec(argv[3]);
        if (r.has_value()) {
            std::cout << "Exported to: " << argv[3] << "\n";
        } else {
            std::cerr << "Error: " << r.error().message() << "\n";
            return 1;
        }
        return 0;

    } else if (command == "import") {
        if (argc < 3) {
            std::cerr << "Error: 'import' requires a spec file\n";
            return 1;
        }

        std::string name;
        for (int i = 3; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--name" && i + 1 < argc) {
                name = argv[++i];
            }
        }

        auto r = mgr.import_spec(argv[2], name);
        if (r.has_value()) {
            std::cout << "Imported garden: " << r.value().spec().name << "\n";
            std::cout << "Location: " << r.value().root_dir() << "\n";
        } else {
            std::cerr << "Error: " << r.error().message() << "\n";
            return 1;
        }
        return 0;

    } else if (command == "clone") {
        if (argc < 4) {
            std::cerr << "Error: 'clone' requires source and destination names\n";
            return 1;
        }

        auto r = mgr.clone(argv[2], argv[3]);
        if (r.has_value()) {
            std::cout << "Cloned " << argv[2] << " -> " << argv[3] << "\n";
            std::cout << "Location: " << r.value().root_dir() << "\n";
        } else {
            std::cerr << "Error: " << r.error().message() << "\n";
            return 1;
        }
        return 0;

    } else if (command == "destroy") {
        if (argc < 3) {
            std::cerr << "Error: 'destroy' requires an environment name\n";
            return 1;
        }

        std::string name = argv[2];

        // Confirm
        std::cerr << "Destroy garden '" << name << "'? This cannot be undone. [y/N] ";
        std::string confirm;
        std::getline(std::cin, confirm);
        if (confirm != "y" && confirm != "Y") {
            std::cerr << "Aborted.\n";
            return 0;
        }

        auto r = mgr.destroy(name);
        if (r.has_value()) {
            std::cout << "Destroyed garden: " << name << "\n";
        } else {
            std::cerr << "Error: " << r.error().message() << "\n";
            return 1;
        }
        return 0;

    } else if (command == "env") {
        if (argc < 3) {
            std::cerr << "Error: 'env' requires an environment name\n";
            return 1;
        }

        auto env = mgr.load(argv[2]);
        if (!env.has_value()) {
            std::cerr << "Error: " << env.error().message() << "\n";
            return 1;
        }

        // Print environment variables that would be set
        const auto& spec = env.value().spec();
        for (const auto& ev : spec.env_vars) {
            std::cout << ev.key << "=" << ev.value << "\n";
        }

        // Print path additions
        std::string paths;
        for (const auto& p : spec.path_prepend) {
            if (!paths.empty()) paths += ":";
            paths += p;
        }
        if (!paths.empty()) {
            std::cout << "PATH=" << paths << ":$PATH\n";
        }
        std::cout << "LD_LIBRARY_PATH=" << env.value().lib_dir() << "\n";

        return 0;

    } else {
        std::cerr << "Error: unknown command '" << command << "'\n";
        print_usage();
        return 1;
    }
}
