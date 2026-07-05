// tools/sysctl-manager/main.cpp
// straylight-sysctl — sysctl preset manager CLI.
#include "sysctl_manager.h"

#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>

static void print_usage() {
    std::cerr
        << "straylight-sysctl — kernel parameter profile manager\n"
        << "\n"
        << "Usage:\n"
        << "  straylight-sysctl apply <profile>        Apply a named sysctl profile\n"
        << "  straylight-sysctl save <name> [--desc D]  Save current state as profile\n"
        << "  straylight-sysctl diff [profile]          Diff current vs profile/default\n"
        << "  straylight-sysctl list                    List available profiles\n"
        << "  straylight-sysctl get <key>               Get a sysctl value\n"
        << "  straylight-sysctl set <key> <value>       Set a sysctl value\n"
        << "  straylight-sysctl rollback                Rollback to pre-apply state\n";
}

static void print_diff(const straylight::SysctlDiff& diff) {
    if (diff.changed.empty() && diff.added.empty() && diff.removed.empty()) {
        std::cout << "No differences found.\n";
        return;
    }

    if (!diff.changed.empty()) {
        std::cout << "\033[33mChanged:\033[0m\n";
        for (const auto& e : diff.changed) {
            std::cout << "  " << e.key << "\n"
                      << "    current: \033[31m" << e.current << "\033[0m\n"
                      << "    target:  \033[32m" << e.target << "\033[0m\n";
        }
    }
    if (!diff.added.empty()) {
        std::cout << "\033[32mNew (in profile, not on system):\033[0m\n";
        for (const auto& e : diff.added) {
            std::cout << "  " << e.key << " = " << e.target << "\n";
        }
    }
    if (!diff.removed.empty()) {
        std::cout << "\033[31mRemoved (on system, not in profile):\033[0m\n";
        for (const auto& e : diff.removed) {
            std::cout << "  " << e.key << " = " << e.current << "\n";
        }
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

    straylight::SysctlManager mgr;

    // -----------------------------------------------------------------------
    // list
    // -----------------------------------------------------------------------
    if (command == "list") {
        auto profiles = mgr.list_profiles();
        if (profiles.empty()) {
            std::cout << "No profiles found in /etc/straylight/sysctl.d/\n";
            return 0;
        }
        std::cout << "Available profiles:\n";
        for (const auto& p : profiles) {
            auto prof = mgr.load_profile(p);
            if (prof.has_value()) {
                std::cout << "  \033[36m" << std::left << std::setw(20) << p << "\033[0m";
                if (!prof.value().description.empty()) {
                    std::cout << " — " << prof.value().description;
                }
                std::cout << " (" << prof.value().params.size() << " params)\n";
            } else {
                std::cout << "  " << p << "\n";
            }
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // get <key>
    // -----------------------------------------------------------------------
    if (command == "get") {
        if (argc < 3) {
            std::cerr << "Error: 'get' requires a key\n";
            return 1;
        }
        auto res = mgr.get(argv[2]);
        if (res.has_value()) {
            std::cout << argv[2] << " = " << res.value() << "\n";
        } else {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // set <key> <value>
    // -----------------------------------------------------------------------
    if (command == "set") {
        if (argc < 4) {
            std::cerr << "Error: 'set' requires a key and value\n";
            return 1;
        }
        auto res = mgr.set(argv[2], argv[3]);
        if (res.has_value()) {
            std::cout << "Set " << argv[2] << " = " << argv[3] << "\n";
        } else {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // apply <profile>
    // -----------------------------------------------------------------------
    if (command == "apply") {
        if (argc < 3) {
            std::cerr << "Error: 'apply' requires a profile name\n";
            return 1;
        }
        std::cout << "Applying profile '" << argv[2] << "'...\n";
        auto res = mgr.apply(argv[2]);
        if (res.has_value()) {
            std::cout << "\033[32mProfile applied successfully.\033[0m\n";
            std::cout << "Use 'straylight-sysctl rollback' to revert.\n";
        } else {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // save <name>
    // -----------------------------------------------------------------------
    if (command == "save") {
        if (argc < 3) {
            std::cerr << "Error: 'save' requires a profile name\n";
            return 1;
        }
        std::string desc;
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "--desc") == 0 && i + 1 < argc) {
                desc = argv[++i];
            }
        }
        std::cout << "Saving current sysctl state as '" << argv[2] << "'...\n";
        auto res = mgr.save_profile(argv[2], desc);
        if (res.has_value()) {
            std::cout << "\033[32mProfile saved.\033[0m\n";
        } else {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // diff [profile]
    // -----------------------------------------------------------------------
    if (command == "diff") {
        if (argc >= 3) {
            auto res = mgr.diff(argv[2]);
            if (res.has_value()) {
                std::cout << "Diff: current vs profile '" << argv[2] << "':\n";
                print_diff(res.value());
            } else {
                std::cerr << "Error: " << res.error() << "\n";
                return 1;
            }
        } else {
            auto res = mgr.diff_default();
            if (res.has_value()) {
                std::cout << "Diff: current vs pre-apply state:\n";
                print_diff(res.value());
            } else {
                std::cerr << "Error: " << res.error() << "\n";
                return 1;
            }
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // rollback
    // -----------------------------------------------------------------------
    if (command == "rollback") {
        std::cout << "Rolling back to pre-apply state...\n";
        auto res = mgr.rollback();
        if (res.has_value()) {
            std::cout << "\033[32mRollback complete.\033[0m\n";
        } else {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        return 0;
    }

    std::cerr << "Unknown command: " << command << "\n";
    print_usage();
    return 1;
}
