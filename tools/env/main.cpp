// tools/env/main.cpp
// straylight-env — environment variable manager CLI.
#include "env_manager.h"

#include <cstring>
#include <iostream>
#include <string>

static void print_usage() {
    std::cerr
        << "straylight-env — environment variable manager\n\n"
        << "Usage:\n"
        << "  straylight-env set <key> <value> [--profile=<p>]\n"
        << "  straylight-env get <key> [--profile=<p>]\n"
        << "  straylight-env list [--profile=<p>]\n"
        << "  straylight-env switch <profile>\n"
        << "  straylight-env diff <profile1> <profile2>\n"
        << "  straylight-env run <cmd> --profile=<p>\n"
        << "  straylight-env profiles\n"
        << "  straylight-env source [--profile=<p>]\n";
}

static std::string parse_profile(int argc, char* argv[], int start = 2) {
    for (int i = start; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--profile=", 0) == 0) return a.substr(10);
    }
    return "";
}

int main(int argc, char* argv[]) {
    if (argc < 2) { print_usage(); return 1; }
    std::string command = argv[1];
    if (command == "--help" || command == "-h") { print_usage(); return 0; }

    straylight::EnvManager mgr;

    if (command == "set") {
        if (argc < 4) { std::cerr << "Error: need key and value\n"; return 1; }
        std::string p = parse_profile(argc, argv, 4);
        auto r = mgr.set(argv[2], argv[3], p);
        if (r.has_value()) {
            std::string pn = p.empty() ? mgr.active_profile() : p;
            std::cout << "Set " << argv[2] << "=" << argv[3] << " in '" << pn << "'\n";
        } else { std::cerr << "Error: " << r.error() << "\n"; return 1; }
        return 0;
    }

    if (command == "get") {
        if (argc < 3) { std::cerr << "Error: need key\n"; return 1; }
        auto r = mgr.get(argv[2], parse_profile(argc, argv, 3));
        if (r.has_value()) std::cout << r.value() << "\n";
        else { std::cerr << "Error: " << r.error() << "\n"; return 1; }
        return 0;
    }

    if (command == "list") {
        std::string p = parse_profile(argc, argv);
        std::string pn = p.empty() ? mgr.active_profile() : p;
        auto vars = mgr.list(p);
        if (vars.empty()) { std::cout << "No variables in '" << pn << "'.\n"; return 0; }
        std::cout << "Profile: \033[36m" << pn << "\033[0m\n";
        for (const auto& [k, v] : vars) std::cout << "  " << k << "=" << v << "\n";
        return 0;
    }

    if (command == "profiles") {
        auto profiles = mgr.list_profiles();
        if (profiles.empty()) { std::cout << "No profiles.\n"; return 0; }
        for (const auto& p : profiles) {
            if (p == mgr.active_profile()) std::cout << "  \033[32m* " << p << "\033[0m (active)\n";
            else std::cout << "    " << p << "\n";
        }
        return 0;
    }

    if (command == "switch") {
        if (argc < 3) { std::cerr << "Error: need profile name\n"; return 1; }
        mgr.switch_profile(argv[2]);
        std::cout << "Switched to '" << argv[2] << "'. Run 'eval $(straylight-env source)' to apply.\n";
        return 0;
    }

    if (command == "diff") {
        if (argc < 4) { std::cerr << "Error: need two profile names\n"; return 1; }
        auto d = mgr.diff(argv[2], argv[3]);
        if (d.changed.empty() && d.only_left.empty() && d.only_right.empty()) {
            std::cout << "No differences.\n"; return 0;
        }
        if (!d.changed.empty()) {
            std::cout << "\033[33mChanged:\033[0m\n";
            for (const auto& e : d.changed)
                std::cout << "  " << e.key << ": \033[31m" << e.left << "\033[0m -> \033[32m" << e.right << "\033[0m\n";
        }
        if (!d.only_left.empty()) {
            std::cout << "\033[31mOnly in " << argv[2] << ":\033[0m\n";
            for (const auto& e : d.only_left) std::cout << "  " << e.key << "=" << e.left << "\n";
        }
        if (!d.only_right.empty()) {
            std::cout << "\033[32mOnly in " << argv[3] << ":\033[0m\n";
            for (const auto& e : d.only_right) std::cout << "  " << e.key << "=" << e.right << "\n";
        }
        return 0;
    }

    if (command == "run") {
        if (argc < 3) { std::cerr << "Error: need command\n"; return 1; }
        std::string p = parse_profile(argc, argv, 3);
        if (p.empty()) p = mgr.active_profile();
        std::string cmd;
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (a.rfind("--profile=", 0) == 0) continue;
            if (!cmd.empty()) cmd += " ";
            cmd += a;
        }
        auto r = mgr.run_with_profile(cmd, p);
        return r.has_value() ? r.value() : 1;
    }

    if (command == "source") {
        std::cout << mgr.source_snippet(parse_profile(argc, argv));
        return 0;
    }

    std::cerr << "Unknown command: " << command << "\n";
    print_usage();
    return 1;
}
