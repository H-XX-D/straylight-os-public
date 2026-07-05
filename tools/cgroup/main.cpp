// tools/cgroup/main.cpp
// CLI front-end for straylight-cgroup — cgroup v2 inspector/manager.

#include "cgroup_manager.h"

#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>

// ---------------------------------------------------------------------------
// Usage
// ---------------------------------------------------------------------------

static void print_usage() {
    std::cerr
        << "straylight-cgroup — cgroup v2 inspector & manager\n"
        << "\n"
        << "Usage:\n"
        << "  straylight-cgroup tree                              Cgroup hierarchy tree\n"
        << "  straylight-cgroup info <path>                       Detailed cgroup info\n"
        << "  straylight-cgroup usage                             Resource usage summary\n"
        << "  straylight-cgroup create <path>                     Create cgroup\n"
        << "  straylight-cgroup remove <path>                     Remove empty cgroup\n"
        << "  straylight-cgroup move <pid> <path>                 Move process to cgroup\n"
        << "  straylight-cgroup limits <path>                     Show resource limits\n"
        << "  straylight-cgroup set <path> [options]              Set resource limits\n"
        << "\n"
        << "Set options:\n"
        << "  --cpu-max=<usec>       CPU quota per period\n"
        << "  --cpu-period=<usec>    CPU period (default 100000)\n"
        << "  --mem-max=<bytes>      Memory hard limit\n"
        << "  --mem-high=<bytes>     Memory soft limit\n"
        << "  --io-max=<spec>        I/O bandwidth limit\n"
        << "  --pids-max=<n>         Max PIDs\n";
}

// ---------------------------------------------------------------------------
// Argument helpers
// ---------------------------------------------------------------------------

static std::string get_arg(int argc, char* argv[],
                            const std::string& prefix, int start = 2) {
    for (int i = start; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind(prefix, 0) == 0) return arg.substr(prefix.size());
    }
    return "";
}

// ---------------------------------------------------------------------------
// Display helpers
// ---------------------------------------------------------------------------

static std::string human_bytes(uint64_t bytes) {
    const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    int idx = 0;
    double val = static_cast<double>(bytes);
    while (val >= 1024.0 && idx < 4) { val /= 1024.0; ++idx; }
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f %s", val, units[idx]);
    return buf;
}

static std::string pad(const std::string& s, size_t width) {
    if (s.size() >= width) return s;
    return s + std::string(width - s.size(), ' ');
}

static void print_tree_node(const straylight::CgroupNode& node) {
    std::string indent(node.depth * 2, ' ');
    std::string mem = human_bytes(node.memory_current);
    std::cout << indent << node.name;
    if (node.process_count > 0)
        std::cout << " (" << node.process_count << " procs, " << mem << ")";
    else if (node.memory_current > 0)
        std::cout << " (" << mem << ")";
    std::cout << "\n";
    for (const auto& child : node.children) print_tree_node(child);
}

// ===========================================================================
// main
// ===========================================================================

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

    straylight::CgroupManager mgr;

    // -----------------------------------------------------------------------
    // tree
    // -----------------------------------------------------------------------
    if (command == "tree") {
        auto res = mgr.tree();
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        print_tree_node(res.value());
        return 0;
    }

    // -----------------------------------------------------------------------
    // info <path>
    // -----------------------------------------------------------------------
    if (command == "info") {
        if (argc < 3) {
            std::cerr << "Error: 'info' requires a cgroup path\n";
            return 1;
        }
        auto res = mgr.info(argv[2]);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        const auto& u = res.value();
        std::cout << "Cgroup: " << u.path << "\n"
                  << "  Processes:    " << u.process_count << "\n"
                  << "  CPU Usage:    " << u.cpu_usage_us << " us\n"
                  << "  Memory:       " << human_bytes(u.memory_current);
        if (u.memory_limit != UINT64_MAX && u.memory_limit > 0)
            std::cout << " / " << human_bytes(u.memory_limit)
                      << " (" << std::fixed << std::setprecision(1)
                      << u.memory_percent << "%)";
        std::cout << "\n"
                  << "  I/O Read:     " << human_bytes(u.io_read_bytes) << "\n"
                  << "  I/O Write:    " << human_bytes(u.io_write_bytes) << "\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // usage
    // -----------------------------------------------------------------------
    if (command == "usage") {
        auto res = mgr.usage();
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        const auto& all = res.value();
        if (all.empty()) {
            std::cout << "No cgroups found.\n";
            return 0;
        }
        std::cout << pad("CGROUP", 30) << pad("PROCS", 8)
                  << pad("MEMORY", 14) << pad("MEM%", 8)
                  << pad("IO READ", 14) << pad("IO WRITE", 14) << "\n"
                  << std::string(88, '-') << "\n";
        for (const auto& u : all) {
            std::cout << pad(u.path, 30)
                      << pad(std::to_string(u.process_count), 8)
                      << pad(human_bytes(u.memory_current), 14);
            if (u.memory_limit != UINT64_MAX && u.memory_limit > 0) {
                char pct[16];
                snprintf(pct, sizeof(pct), "%.1f%%", u.memory_percent);
                std::cout << pad(pct, 8);
            } else {
                std::cout << pad("-", 8);
            }
            std::cout << pad(human_bytes(u.io_read_bytes), 14)
                      << pad(human_bytes(u.io_write_bytes), 14) << "\n";
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // create <path>
    // -----------------------------------------------------------------------
    if (command == "create") {
        if (argc < 3) {
            std::cerr << "Error: 'create' requires a cgroup path\n";
            return 1;
        }
        auto res = mgr.create(argv[2]);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Cgroup '" << argv[2] << "' created.\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // remove <path>
    // -----------------------------------------------------------------------
    if (command == "remove") {
        if (argc < 3) {
            std::cerr << "Error: 'remove' requires a cgroup path\n";
            return 1;
        }
        auto res = mgr.remove(argv[2]);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Cgroup '" << argv[2] << "' removed.\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // move <pid> <path>
    // -----------------------------------------------------------------------
    if (command == "move") {
        if (argc < 4) {
            std::cerr << "Error: 'move' requires <pid> and <cgroup path>\n";
            return 1;
        }
        int pid = std::atoi(argv[2]);
        if (pid <= 0) {
            std::cerr << "Error: invalid PID '" << argv[2] << "'\n";
            return 1;
        }
        auto res = mgr.move(pid, argv[3]);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Process " << pid << " moved to '" << argv[3] << "'.\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // limits <path>
    // -----------------------------------------------------------------------
    if (command == "limits") {
        if (argc < 3) {
            std::cerr << "Error: 'limits' requires a cgroup path\n";
            return 1;
        }
        auto res = mgr.get_limits(argv[2]);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        const auto& lim = res.value();
        std::cout << "Resource Limits for " << argv[2] << ":\n";
        if (lim.cpu_max >= 0)
            std::cout << "  CPU Max:      " << lim.cpu_max << " / " << lim.cpu_period << " us\n";
        else
            std::cout << "  CPU Max:      unlimited\n";
        if (lim.memory_max >= 0)
            std::cout << "  Memory Max:   " << human_bytes(static_cast<uint64_t>(lim.memory_max)) << "\n";
        else
            std::cout << "  Memory Max:   unlimited\n";
        if (lim.memory_high >= 0)
            std::cout << "  Memory High:  " << human_bytes(static_cast<uint64_t>(lim.memory_high)) << "\n";
        else
            std::cout << "  Memory High:  unlimited\n";
        if (lim.pids_max >= 0)
            std::cout << "  PIDs Max:     " << lim.pids_max << "\n";
        else
            std::cout << "  PIDs Max:     unlimited\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // set <path> [options]
    // -----------------------------------------------------------------------
    if (command == "set") {
        if (argc < 3) {
            std::cerr << "Error: 'set' requires a cgroup path\n";
            return 1;
        }
        straylight::CgroupLimits limits;
        std::string val;

        val = get_arg(argc, argv, "--cpu-max=", 3);
        if (!val.empty()) limits.cpu_max = std::stoll(val);

        val = get_arg(argc, argv, "--cpu-period=", 3);
        if (!val.empty()) limits.cpu_period = std::stoll(val);

        val = get_arg(argc, argv, "--mem-max=", 3);
        if (!val.empty()) limits.memory_max = std::stoll(val);

        val = get_arg(argc, argv, "--mem-high=", 3);
        if (!val.empty()) limits.memory_high = std::stoll(val);

        val = get_arg(argc, argv, "--io-max=", 3);
        if (!val.empty()) limits.io_max = val;

        val = get_arg(argc, argv, "--pids-max=", 3);
        if (!val.empty()) limits.pids_max = std::stoi(val);

        auto res = mgr.set_limits(argv[2], limits);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Limits updated for '" << argv[2] << "'.\n";
        return 0;
    }

    std::cerr << "Error: unknown command '" << command << "'\n";
    print_usage();
    return 1;
}
