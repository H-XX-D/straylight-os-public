// tools/port/main.cpp
// CLI front-end for straylight-port — port manager.

#include "port_manager.h"

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>

static void print_usage() {
    std::cerr
        << "straylight-port — port manager CLI\n"
        << "\n"
        << "Usage:\n"
        << "  straylight-port list [--listening]           List ports\n"
        << "  straylight-port who <port>                   Who is using a port\n"
        << "  straylight-port kill <port>                  Kill process on port\n"
        << "  straylight-port reserve <port> <service>     Reserve port for service\n"
        << "  straylight-port unreserve <port>             Unreserve port\n"
        << "  straylight-port conflicts                    Check reservation conflicts\n"
        << "  straylight-port free <start>-<end>           Find free ports in range\n"
        << "  straylight-port reservations                 List all reservations\n";
}

static bool has_flag(int argc, char* argv[], const std::string& flag, int start = 2) {
    for (int i = start; i < argc; ++i) {
        if (std::string(argv[i]) == flag) return true;
    }
    return false;
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

    straylight::PortManager mgr;

    // -----------------------------------------------------------------------
    // list [--listening]
    // -----------------------------------------------------------------------
    if (command == "list") {
        bool listening = has_flag(argc, argv, "--listening");
        auto res = mgr.list(listening);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }

        const auto& ports = res.value();
        if (ports.empty()) {
            std::cout << "No " << (listening ? "listening " : "") << "ports found.\n";
            return 0;
        }

        std::cout << std::left
                  << std::setw(8) << "PORT"
                  << std::setw(8) << "PROTO"
                  << std::setw(12) << "STATE"
                  << std::setw(8) << "PID"
                  << std::setw(16) << "PROCESS"
                  << "ADDRESS\n";
        std::cout << std::string(68, '-') << "\n";

        for (const auto& p : ports) {
            std::cout << std::left
                      << std::setw(8) << p.port
                      << std::setw(8) << p.protocol
                      << std::setw(12) << p.state
                      << std::setw(8) << (p.pid > 0 ? std::to_string(p.pid) : "-")
                      << std::setw(16) << (p.process_name.empty() ? "-" : p.process_name)
                      << p.local_addr << "\n";
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // who <port>
    // -----------------------------------------------------------------------
    if (command == "who") {
        if (argc < 3) {
            std::cerr << "Error: 'who' requires a port number\n";
            return 1;
        }
        uint16_t port = static_cast<uint16_t>(std::atoi(argv[2]));
        auto res = mgr.who(port);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }

        const auto& e = res.value();
        std::cout << "Port " << e.port << "/" << e.protocol << "\n"
                  << "  State:   " << e.state << "\n"
                  << "  PID:     " << e.pid << "\n"
                  << "  Process: " << e.process_name << "\n"
                  << "  User:    " << e.user << "\n"
                  << "  Local:   " << e.local_addr << ":" << e.port << "\n"
                  << "  Remote:  " << e.remote_addr << "\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // kill <port>
    // -----------------------------------------------------------------------
    if (command == "kill") {
        if (argc < 3) {
            std::cerr << "Error: 'kill' requires a port number\n";
            return 1;
        }
        uint16_t port = static_cast<uint16_t>(std::atoi(argv[2]));

        // First show who's there
        auto who_res = mgr.who(port);
        if (who_res.has_value()) {
            const auto& e = who_res.value();
            std::cout << "Killing " << e.process_name << " (PID " << e.pid
                      << ") on port " << port << "...\n";
        }

        auto res = mgr.kill(port);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Process on port " << port << " terminated.\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // reserve <port> <service> [--proto=tcp] [--desc=X]
    // -----------------------------------------------------------------------
    if (command == "reserve") {
        if (argc < 4) {
            std::cerr << "Error: 'reserve' requires <port> <service>\n";
            return 1;
        }
        uint16_t port = static_cast<uint16_t>(std::atoi(argv[2]));
        std::string service = argv[3];

        std::string proto = "tcp";
        std::string desc;
        for (int i = 4; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg.rfind("--proto=", 0) == 0) proto = arg.substr(8);
            if (arg.rfind("--desc=", 0) == 0) desc = arg.substr(7);
        }

        auto res = mgr.reserve(port, service, proto, desc);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Port " << port << "/" << proto << " reserved for '" << service << "'\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // unreserve <port>
    // -----------------------------------------------------------------------
    if (command == "unreserve") {
        if (argc < 3) {
            std::cerr << "Error: 'unreserve' requires a port number\n";
            return 1;
        }
        uint16_t port = static_cast<uint16_t>(std::atoi(argv[2]));
        auto res = mgr.unreserve(port);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }
        std::cout << "Port " << port << " unreserved.\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // conflicts
    // -----------------------------------------------------------------------
    if (command == "conflicts") {
        auto res = mgr.conflicts();
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }

        const auto& conflicts = res.value();
        if (conflicts.empty()) {
            std::cout << "No port conflicts detected.\n";
            return 0;
        }

        std::cout << "\033[31mPort Conflicts:\033[0m\n\n";
        for (const auto& c : conflicts) {
            std::cout << "  Port " << c.port << ": reserved for '"
                      << c.reserved_for << "' but used by '"
                      << c.actual_process << "' (PID " << c.actual_pid << ")\n";
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // free <start>-<end>
    // -----------------------------------------------------------------------
    if (command == "free") {
        if (argc < 3) {
            std::cerr << "Error: 'free' requires a range (e.g., 8000-9000)\n";
            return 1;
        }
        std::string range = argv[2];
        auto dash = range.find('-');
        if (dash == std::string::npos) {
            std::cerr << "Error: invalid range format (use start-end)\n";
            return 1;
        }

        uint16_t start = static_cast<uint16_t>(std::atoi(range.substr(0, dash).c_str()));
        uint16_t end = static_cast<uint16_t>(std::atoi(range.substr(dash + 1).c_str()));

        auto res = mgr.free_ports(start, end);
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }

        const auto& free = res.value();
        std::cout << "Free ports in " << start << "-" << end << ": "
                  << free.size() << " available\n";

        // Show first 20
        int shown = 0;
        for (const auto& p : free) {
            std::cout << "  " << p;
            if (++shown >= 20) {
                std::cout << "  ... and " << (free.size() - 20) << " more";
                break;
            }
        }
        std::cout << "\n";
        return 0;
    }

    // -----------------------------------------------------------------------
    // reservations
    // -----------------------------------------------------------------------
    if (command == "reservations") {
        auto res = mgr.reservations();
        if (!res.has_value()) {
            std::cerr << "Error: " << res.error() << "\n";
            return 1;
        }

        const auto& reservations = res.value();
        if (reservations.empty()) {
            std::cout << "No port reservations.\n";
            return 0;
        }

        std::cout << std::left
                  << std::setw(8) << "PORT"
                  << std::setw(8) << "PROTO"
                  << std::setw(24) << "SERVICE"
                  << "DESCRIPTION\n";
        std::cout << std::string(60, '-') << "\n";

        for (const auto& r : reservations) {
            std::cout << std::left
                      << std::setw(8) << r.port
                      << std::setw(8) << r.protocol
                      << std::setw(24) << r.service_name
                      << r.description << "\n";
        }
        return 0;
    }

    std::cerr << "Error: unknown command '" << command << "'\n";
    print_usage();
    return 1;
}
