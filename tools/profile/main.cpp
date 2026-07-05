// tools/profile/main.cpp
// straylight-profile — system profiler CLI.
#include "profiler.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

static void print_usage() {
    std::cerr
        << "straylight-profile — comprehensive system profiler\n"
        << "\n"
        << "Usage:\n"
        << "  straylight-profile report [--format=text|json|html] [--output <file>]\n"
        << "  straylight-profile compare <report1.json> <report2.json>\n"
        << "  straylight-profile export [--output <file>]\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        // Default: run report in text mode
        straylight::Profiler profiler;
        auto profile = profiler.collect();
        std::cout << straylight::Profiler::format_text(profile);
        return 0;
    }

    std::string command = argv[1];
    if (command == "--help" || command == "-h") {
        print_usage();
        return 0;
    }

    // -----------------------------------------------------------------------
    // report
    // -----------------------------------------------------------------------
    if (command == "report") {
        std::string format = "text";
        std::string output;

        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg.rfind("--format=", 0) == 0) {
                format = arg.substr(9);
            } else if (arg == "--output" && i + 1 < argc) {
                output = argv[++i];
            }
        }

        straylight::Profiler profiler;
        auto profile = profiler.collect();

        std::string result;
        if (format == "json") {
            result = straylight::Profiler::format_json(profile);
        } else if (format == "html") {
            result = straylight::Profiler::format_html(profile);
        } else {
            result = straylight::Profiler::format_text(profile);
        }

        if (!output.empty()) {
            std::ofstream ofs(output);
            if (!ofs) {
                std::cerr << "Error: cannot write to " << output << "\n";
                return 1;
            }
            ofs << result;
            std::cout << "Report saved to " << output << "\n";
        } else {
            std::cout << result;
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // compare
    // -----------------------------------------------------------------------
    if (command == "compare") {
        if (argc < 4) {
            std::cerr << "Error: 'compare' requires two report files\n";
            return 1;
        }

        auto a_res = straylight::Profiler::load(argv[2]);
        if (!a_res.has_value()) {
            std::cerr << "Error loading " << argv[2] << ": " << a_res.error() << "\n";
            return 1;
        }

        auto b_res = straylight::Profiler::load(argv[3]);
        if (!b_res.has_value()) {
            std::cerr << "Error loading " << argv[3] << ": " << b_res.error() << "\n";
            return 1;
        }

        auto comp = straylight::Profiler::compare(a_res.value(), b_res.value());

        std::cout << "\033[1;36m=== Profile Comparison ===\033[0m\n";
        std::cout << "Old: " << a_res.value().timestamp << " (" << a_res.value().hostname << ")\n";
        std::cout << "New: " << b_res.value().timestamp << " (" << b_res.value().hostname << ")\n\n";

        if (comp.old_health >= 0 && comp.new_health >= 0) {
            std::string arrow = comp.new_health > comp.old_health ? "\033[32m+" :
                                comp.new_health < comp.old_health ? "\033[31m" : "\033[33m=";
            std::cout << "Health: " << comp.old_health << " -> " << arrow
                      << comp.new_health << "\033[0m\n\n";
        }

        if (comp.changes.empty()) {
            std::cout << "No differences found.\n";
        } else {
            std::string current_cat;
            for (const auto& d : comp.changes) {
                if (d.category != current_cat) {
                    current_cat = d.category;
                    std::cout << "\033[1;33m" << current_cat << "\033[0m\n";
                }
                std::cout << "  " << d.name << ": "
                          << "\033[31m" << d.old_value << "\033[0m"
                          << " -> "
                          << "\033[32m" << d.new_value << "\033[0m\n";
            }
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // export
    // -----------------------------------------------------------------------
    if (command == "export") {
        std::string output = "system-profile.json";
        for (int i = 2; i < argc; ++i) {
            if (std::strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
                output = argv[++i];
            }
        }

        straylight::Profiler profiler;
        auto profile = profiler.collect();
        auto res = straylight::Profiler::save(profile, output);
        if (res.has_value()) {
            std::cout << "Profile exported to " << output << "\n";
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
