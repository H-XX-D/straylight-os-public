// tools/shield/main.cpp
// CLI front-end for straylight-shield -- security hardening & audit.

#include "auditor.h"
#include "hardener.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

static void print_usage() {
    std::cerr
        << "straylight-shield -- security hardening & audit\n"
        << "\n"
        << "Usage:\n"
        << "  straylight-shield audit                              Full security audit\n"
        << "  straylight-shield harden [--level basic|moderate|strict]  Apply hardening\n"
        << "  straylight-shield preview [--level basic|moderate|strict] Preview changes\n"
        << "  straylight-shield check <category>                   Check specific area\n"
        << "  straylight-shield report [--output <file>]           Generate report\n"
        << "\n"
        << "Categories: filesystem, network, users, kernel, services, ssh, updates\n";
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

    // -----------------------------------------------------------------------
    // audit
    // -----------------------------------------------------------------------
    if (command == "audit") {
        straylight::Auditor auditor;
        std::cout << "Running full security audit...\n";
        auto res = auditor.full_audit();
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << straylight::Auditor::format_report(res.value());
        return res.value().score >= 60 ? 0 : 1;
    }

    // -----------------------------------------------------------------------
    // harden
    // -----------------------------------------------------------------------
    if (command == "harden") {
        std::string level_str = "basic";
        for (int i = 2; i < argc; ++i) {
            if (std::strcmp(argv[i], "--level") == 0 && i + 1 < argc) {
                level_str = argv[++i];
            }
        }

        auto level_res = straylight::Hardener::parse_level(level_str);
        if (!level_res.has_value()) {
            std::cerr << "Error: " << level_res.error() << "\n";
            return 1;
        }

        straylight::Hardener hardener;
        std::cout << "Applying " << level_str << " hardening...\n";
        auto res = hardener.harden(level_res.value());
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << straylight::Hardener::format_report(res.value());
        return 0;
    }

    // -----------------------------------------------------------------------
    // preview
    // -----------------------------------------------------------------------
    if (command == "preview") {
        std::string level_str = "basic";
        for (int i = 2; i < argc; ++i) {
            if (std::strcmp(argv[i], "--level") == 0 && i + 1 < argc) {
                level_str = argv[++i];
            }
        }

        auto level_res = straylight::Hardener::parse_level(level_str);
        if (!level_res.has_value()) {
            std::cerr << "Error: " << level_res.error() << "\n";
            return 1;
        }

        straylight::Hardener hardener;
        std::cout << "Preview of " << level_str << " hardening (no changes applied):\n";
        auto res = hardener.preview(level_res.value());
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << straylight::Hardener::format_report(res.value());
        return 0;
    }

    // -----------------------------------------------------------------------
    // check
    // -----------------------------------------------------------------------
    if (command == "check") {
        if (argc < 3) {
            std::cerr << "Error: 'check' requires <category>\n";
            std::cerr << "Categories: filesystem, network, users, kernel, "
                         "services, ssh, updates\n";
            return 1;
        }
        std::string category = argv[2];

        straylight::Auditor auditor;
        auto res = auditor.check_category(category);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << straylight::Auditor::format_report(res.value());
        return 0;
    }

    // -----------------------------------------------------------------------
    // report
    // -----------------------------------------------------------------------
    if (command == "report") {
        std::string output_file;
        for (int i = 2; i < argc; ++i) {
            if ((std::strcmp(argv[i], "--output") == 0 ||
                 std::strcmp(argv[i], "-o") == 0) &&
                i + 1 < argc) {
                output_file = argv[++i];
            }
        }

        straylight::Auditor auditor;
        std::cout << "Generating security report...\n";
        auto res = auditor.full_audit();
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }

        std::string report = straylight::Auditor::format_report(res.value());

        if (output_file.empty()) {
            std::cout << report;
        } else {
            std::ofstream out(output_file);
            if (!out.is_open()) {
                std::cerr << "Error: cannot write to " << output_file << "\n";
                return 1;
            }
            out << report;
            out.close();
            std::cout << "Report written to " << output_file << "\n";
        }
        return 0;
    }

    std::cerr << "Error: unknown command '" << command << "'\n";
    print_usage();
    return 1;
}
