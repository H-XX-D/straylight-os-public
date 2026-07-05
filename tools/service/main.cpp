// tools/service/main.cpp
// straylight-service — unified service manager CLI.
#include "service_manager.h"

#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

static void print_usage() {
    std::cerr
        << "straylight-service — unified service manager\n\n"
        << "Usage:\n"
        << "  straylight-service list [--straylight|--system|--all]\n"
        << "  straylight-service status <service>\n"
        << "  straylight-service start|stop|restart <service>\n"
        << "  straylight-service logs <service> [--follow] [-n <lines>]\n"
        << "  straylight-service deps <service>\n"
        << "  straylight-service resources <service>\n"
        << "  straylight-service create <name> [--desc <description>]\n";
}

static std::string format_bytes(uint64_t bytes) {
    const char* u[] = {"B", "KB", "MB", "GB"};
    int i = 0; double s = static_cast<double>(bytes);
    while (s >= 1024.0 && i < 3) { s /= 1024.0; i++; }
    std::ostringstream o; o << std::fixed << std::setprecision(1) << s << " " << u[i];
    return o.str();
}

static std::string colorize(const std::string& state) {
    if (state == "active") return "\033[32m" + state + "\033[0m";
    if (state == "failed") return "\033[31m" + state + "\033[0m";
    return "\033[90m" + state + "\033[0m";
}

int main(int argc, char* argv[]) {
    if (argc < 2) { print_usage(); return 1; }
    std::string cmd = argv[1];
    if (cmd == "--help" || cmd == "-h") { print_usage(); return 0; }

    straylight::ServiceManager mgr;

    if (cmd == "list") {
        std::string filter = "all";
        for (int i = 2; i < argc; ++i) {
            if (std::strcmp(argv[i], "--straylight") == 0) filter = "straylight";
            else if (std::strcmp(argv[i], "--system") == 0) filter = "system";
        }
        auto svcs = mgr.list(filter);
        std::cout << "\033[1;36mStrayLight Services:\033[0m\n";
        bool has_sl = false;
        for (const auto& s : svcs) {
            if (!s.is_straylight) continue;
            has_sl = true;
            printf("  %-35s %-12s %s\n", s.name.c_str(), colorize(s.state).c_str(), s.description.c_str());
        }
        if (!has_sl) std::cout << "  (none)\n";
        if (filter != "straylight") {
            std::cout << "\n\033[1;33mSystem Services:\033[0m\n";
            int cnt = 0;
            for (const auto& s : svcs) {
                if (s.is_straylight || s.state == "inactive") continue;
                printf("  %-35s %-12s %s\n", s.name.c_str(), colorize(s.state).c_str(), s.description.c_str());
                if (++cnt >= 50) { std::cout << "  ...\n"; break; }
            }
        }
        return 0;
    }

    if (cmd == "status") {
        if (argc < 3) { std::cerr << "Error: need service name\n"; return 1; }
        auto r = mgr.status(argv[2]);
        if (!r.has_value()) { std::cerr << "Error: " << r.error() << "\n"; return 1; }
        const auto& i = r.value();
        std::cout << "Service:     " << i.name;
        if (i.is_straylight) std::cout << " \033[36m[StrayLight]\033[0m";
        std::cout << "\nDescription: " << i.description
                  << "\nState:       " << colorize(i.state) << " (" << i.sub_state << ")"
                  << "\nPID:         " << i.main_pid
                  << "\nStarted:     " << i.started_at << "\n";
        if (i.memory_bytes > 0) std::cout << "Memory:      " << format_bytes(i.memory_bytes) << "\n";
        return 0;
    }

    if (cmd == "start" || cmd == "stop" || cmd == "restart") {
        if (argc < 3) { std::cerr << "Error: need service name\n"; return 1; }
        auto r = (cmd == "start") ? mgr.start(argv[2]) :
                 (cmd == "stop")  ? mgr.stop(argv[2]) : mgr.restart(argv[2]);
        if (r.has_value()) std::cout << "\033[32m" << cmd << " " << argv[2] << " — OK\033[0m\n";
        else { std::cerr << "\033[31m" << cmd << " FAILED: " << r.error() << "\033[0m\n"; return 1; }
        return 0;
    }

    if (cmd == "logs") {
        if (argc < 3) { std::cerr << "Error: need service name\n"; return 1; }
        int lines = 50; bool follow = false;
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "--follow") == 0 || std::strcmp(argv[i], "-f") == 0) follow = true;
            else if (std::strcmp(argv[i], "-n") == 0 && i+1 < argc) lines = std::stoi(argv[++i]);
        }
        auto r = mgr.logs(argv[2], lines, follow);
        if (r.has_value()) std::cout << r.value() << "\n";
        else { std::cerr << "Error: " << r.error() << "\n"; return 1; }
        return 0;
    }

    if (cmd == "deps") {
        if (argc < 3) { std::cerr << "Error: need service name\n"; return 1; }
        auto deps = mgr.dependencies(argv[2]);
        if (deps.empty()) { std::cout << "No dependencies.\n"; return 0; }
        std::string ct;
        for (const auto& d : deps) {
            if (d.type != ct) { ct = d.type; std::cout << "\n  \033[1;33m" << ct << ":\033[0m\n"; }
            std::cout << "    " << d.name << "\n";
        }
        return 0;
    }

    if (cmd == "resources") {
        if (argc < 3) { std::cerr << "Error: need service name\n"; return 1; }
        auto r = mgr.resources(argv[2]);
        if (!r.has_value()) { std::cerr << "Error: " << r.error() << "\n"; return 1; }
        const auto& res = r.value();
        std::cout << "Resources for " << res.name << ":\n"
                  << "  Memory (current): " << format_bytes(res.memory_current) << "\n";
        if (res.memory_peak > 0) std::cout << "  Memory (peak):    " << format_bytes(res.memory_peak) << "\n";
        if (res.task_count > 0) std::cout << "  Tasks:            " << res.task_count << "\n";
        return 0;
    }

    if (cmd == "create") {
        if (argc < 3) { std::cerr << "Error: need name\n"; return 1; }
        std::string desc;
        for (int i = 3; i < argc; ++i)
            if (std::strcmp(argv[i], "--desc") == 0 && i+1 < argc) desc = argv[++i];
        auto r = mgr.create_service(argv[2], desc);
        if (r.has_value()) {
            std::cout << "\033[32mService template created for straylight-" << argv[2] << "\033[0m\n"
                      << "Run 'systemctl daemon-reload' then 'straylight-service start straylight-"
                      << argv[2] << "'\n";
        } else { std::cerr << "Error: " << r.error() << "\n"; return 1; }
        return 0;
    }

    std::cerr << "Unknown command: " << cmd << "\n";
    print_usage();
    return 1;
}
