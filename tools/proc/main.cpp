// tools/proc/main.cpp
// CLI front-end for straylight-proc — process inspector.

#include "proc_inspector.h"

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>

static void print_usage() {
    std::cerr
        << "straylight-proc — process inspector CLI\n"
        << "\n"
        << "Usage:\n"
        << "  straylight-proc info <pid>                   Detailed process info\n"
        << "  straylight-proc tree                         Process hierarchy tree\n"
        << "  straylight-proc files <pid>                  Open file descriptors\n"
        << "  straylight-proc mem <pid>                    Memory map\n"
        << "  straylight-proc net <pid>                    Network connections\n"
        << "  straylight-proc env <pid>                    Environment variables\n"
        << "  straylight-proc signal <pid> <sig>           Send signal\n"
        << "  straylight-proc nice <pid> <level>           Adjust niceness\n"
        << "  straylight-proc find <name>                  Find process by name\n";
}

static std::string human_bytes(uint64_t bytes) {
    const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    int idx = 0;
    double val = static_cast<double>(bytes);
    while (val >= 1024.0 && idx < 4) { val /= 1024.0; ++idx; }
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f %s", val, units[idx]);
    return buf;
}

static void print_tree(const straylight::ProcTreeNode& node, const std::string& prefix = "",
                        bool last = true) {
    std::string connector = last ? "`-- " : "|-- ";
    std::cout << prefix << connector << node.name << "(" << node.pid << ")"
              << " [" << node.state_char << "]\n";

    std::string child_prefix = prefix + (last ? "    " : "|   ");
    for (size_t i = 0; i < node.children.size(); ++i) {
        print_tree(node.children[i], child_prefix, i == node.children.size() - 1);
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

    straylight::ProcInspector inspector;

    // -----------------------------------------------------------------------
    // info <pid>
    // -----------------------------------------------------------------------
    if (command == "info") {
        if (argc < 3) {
            std::cerr << "Error: 'info' requires a PID\n";
            return 1;
        }
        int pid = std::atoi(argv[2]);
        auto res = inspector.info(pid);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }

        const auto& p = res.value();
        std::cout << "\033[1mProcess " << p.pid << "\033[0m\n"
                  << std::string(50, '-') << "\n"
                  << "  Name:     " << p.name << "\n"
                  << "  State:    " << p.state << " (" << p.state_char << ")\n"
                  << "  PID:      " << p.pid << "\n"
                  << "  PPID:     " << p.ppid << "\n"
                  << "  User:     " << p.user << " (uid=" << p.uid << ")\n"
                  << "  Threads:  " << p.threads << "\n"
                  << "  Nice:     " << p.nice << "\n"
                  << "  VM Size:  " << human_bytes(p.vm_size) << "\n"
                  << "  RSS:      " << human_bytes(p.vm_rss) << "\n"
                  << "  Exe:      " << p.exe << "\n"
                  << "  CWD:      " << p.cwd << "\n"
                  << "  Cmdline:  " << p.cmdline << "\n";
        if (!p.cgroup.empty()) {
            std::cout << "  Cgroup:   " << p.cgroup << "\n";
        }

        // IO stats
        auto io_res = inspector.io(pid);
        if (io_res.has_value()) {
            std::cout << "\n  IO:\n"
                      << "    Read:    " << human_bytes(io_res.value().read_bytes) << "\n"
                      << "    Write:   " << human_bytes(io_res.value().write_bytes) << "\n"
                      << "    Syscalls: " << io_res.value().read_syscalls << " r / "
                      << io_res.value().write_syscalls << " w\n";
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // tree
    // -----------------------------------------------------------------------
    if (command == "tree") {
        auto res = inspector.tree();
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        print_tree(res.value());
        return 0;
    }

    // -----------------------------------------------------------------------
    // files <pid>
    // -----------------------------------------------------------------------
    if (command == "files") {
        if (argc < 3) {
            std::cerr << "Error: 'files' requires a PID\n";
            return 1;
        }
        int pid = std::atoi(argv[2]);
        auto res = inspector.files(pid);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }

        std::cout << std::left
                  << std::setw(6) << "FD"
                  << std::setw(14) << "TYPE"
                  << std::setw(6) << "MODE"
                  << "PATH\n";
        std::cout << std::string(70, '-') << "\n";

        for (const auto& f : res.value()) {
            std::cout << std::left
                      << std::setw(6) << f.fd
                      << std::setw(14) << f.type
                      << std::setw(6) << f.mode
                      << f.path << "\n";
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // mem <pid>
    // -----------------------------------------------------------------------
    if (command == "mem") {
        if (argc < 3) {
            std::cerr << "Error: 'mem' requires a PID\n";
            return 1;
        }
        int pid = std::atoi(argv[2]);
        auto res = inspector.mem(pid);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }

        uint64_t total_size = 0;
        std::cout << std::left
                  << std::setw(30) << "ADDRESS"
                  << std::setw(6) << "PERM"
                  << std::setw(12) << "SIZE"
                  << "PATH\n";
        std::cout << std::string(80, '-') << "\n";

        for (const auto& r : res.value()) {
            total_size += r.size;
            std::string name = r.pathname.empty() ? "[anon]" : r.pathname;
            if (name.size() > 40) name = "..." + name.substr(name.size() - 37);

            std::cout << std::left
                      << std::setw(30) << r.address
                      << std::setw(6) << r.perms
                      << std::setw(12) << human_bytes(r.size)
                      << name << "\n";
        }
        std::cout << "\nTotal mapped: " << human_bytes(total_size)
                  << " (" << res.value().size() << " regions)\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // net <pid>
    // -----------------------------------------------------------------------
    if (command == "net") {
        if (argc < 3) {
            std::cerr << "Error: 'net' requires a PID\n";
            return 1;
        }
        int pid = std::atoi(argv[2]);
        auto res = inspector.net(pid);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }

        const auto& conns = res.value();
        if (conns.empty()) {
            std::cout << "No network connections for PID " << pid << "\n";
            return 0;
        }

        std::cout << std::left
                  << std::setw(8) << "PROTO"
                  << std::setw(24) << "LOCAL"
                  << std::setw(24) << "REMOTE"
                  << "STATE\n";
        std::cout << std::string(68, '-') << "\n";

        for (const auto& c : conns) {
            std::string local = c.local_addr + ":" + std::to_string(c.local_port);
            std::string remote = c.remote_addr + ":" + std::to_string(c.remote_port);

            std::cout << std::left
                      << std::setw(8) << c.protocol
                      << std::setw(24) << local
                      << std::setw(24) << remote
                      << c.state << "\n";
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // env <pid>
    // -----------------------------------------------------------------------
    if (command == "env") {
        if (argc < 3) {
            std::cerr << "Error: 'env' requires a PID\n";
            return 1;
        }
        int pid = std::atoi(argv[2]);
        auto res = inspector.env(pid);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }

        for (const auto& [key, value] : res.value()) {
            std::cout << "\033[33m" << key << "\033[0m=" << value << "\n";
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // signal <pid> <sig>
    // -----------------------------------------------------------------------
    if (command == "signal") {
        if (argc < 4) {
            std::cerr << "Error: 'signal' requires <pid> <signal>\n";
            return 1;
        }
        int pid = std::atoi(argv[2]);
        int sig = std::atoi(argv[3]);

        auto res = inspector.signal(pid, sig);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Signal " << sig << " sent to PID " << pid << "\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // nice <pid> <level>
    // -----------------------------------------------------------------------
    if (command == "nice") {
        if (argc < 4) {
            std::cerr << "Error: 'nice' requires <pid> <level>\n";
            return 1;
        }
        int pid = std::atoi(argv[2]);
        int level = std::atoi(argv[3]);

        auto res = inspector.renice(pid, level);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "PID " << pid << " renice to " << level << "\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // find <name>
    // -----------------------------------------------------------------------
    if (command == "find") {
        if (argc < 3) {
            std::cerr << "Error: 'find' requires a process name\n";
            return 1;
        }

        std::string name;
        for (int i = 2; i < argc; ++i) {
            if (i > 2) name += " ";
            name += argv[i];
        }

        auto res = inspector.find(name);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }

        const auto& procs = res.value();
        if (procs.empty()) {
            std::cout << "No processes matching '" << name << "'\n";
            return 0;
        }

        std::cout << std::left
                  << std::setw(8) << "PID"
                  << std::setw(10) << "USER"
                  << std::setw(10) << "STATE"
                  << std::setw(12) << "RSS"
                  << "COMMAND\n";
        std::cout << std::string(70, '-') << "\n";

        for (const auto& p : procs) {
            std::string cmd = p.cmdline.empty() ? p.name : p.cmdline;
            if (cmd.size() > 40) cmd = cmd.substr(0, 37) + "...";

            std::cout << std::left
                      << std::setw(8) << p.pid
                      << std::setw(10) << p.user
                      << std::setw(10) << p.state
                      << std::setw(12) << human_bytes(p.vm_rss)
                      << cmd << "\n";
        }
        return 0;
    }

    std::cerr << "Error: unknown command '" << command << "'\n";
    print_usage();
    return 1;
}
