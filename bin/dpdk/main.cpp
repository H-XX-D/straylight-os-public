// bin/dpdk/main.cpp
// CLI tool: straylight-dpdk <init|start|stats|stop> [options]
#include "port.h"
#include "pipeline.h"
#include "flow.h"
#include "tensor_transport.h"

#include <straylight/result.h>
#include <straylight/error.h>
#include <straylight/log.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace straylight;
using namespace straylight::dpdk;

namespace {

struct Options {
    std::string              command;   // init | start | stats | stop
    uint16_t                 port_id  = 0;
    uint16_t                 nb_rx    = 1;
    uint16_t                 nb_tx    = 1;
    std::vector<std::string> eal_args;
};

void print_usage(const char* argv0) {
    std::cerr << "Usage:\n"
              << "  " << argv0 << " init [--eal <args...>]\n"
              << "  " << argv0 << " start --port <id> [--rxq <n>] [--txq <n>] [--eal <args...>]\n"
              << "  " << argv0 << " stats --port <id> [--eal <args...>]\n"
              << "  " << argv0 << " stop  --port <id> [--eal <args...>]\n";
}

Result<Options, std::string> parse_args(int argc, char* argv[]) {
    if (argc < 2) {
        return Result<Options, std::string>::error("No command specified");
    }

    Options opts;
    opts.command = argv[1];

    if (opts.command != "init" && opts.command != "start" &&
        opts.command != "stats" && opts.command != "stop") {
        return Result<Options, std::string>::error("Unknown command: " + opts.command);
    }

    bool parsing_eal = false;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];

        if (parsing_eal) {
            opts.eal_args.push_back(arg);
            continue;
        }

        if (arg == "--port" && i + 1 < argc) {
            opts.port_id = static_cast<uint16_t>(std::stoul(argv[++i]));
        } else if (arg == "--rxq" && i + 1 < argc) {
            opts.nb_rx = static_cast<uint16_t>(std::stoul(argv[++i]));
        } else if (arg == "--txq" && i + 1 < argc) {
            opts.nb_tx = static_cast<uint16_t>(std::stoul(argv[++i]));
        } else if (arg == "--eal") {
            parsing_eal = true;
        } else {
            return Result<Options, std::string>::error("Unknown option: " + arg);
        }
    }

    return Result<Options, std::string>::ok(std::move(opts));
}

Result<void, SLError> run(const Options& opts) {
    PortManager pm;

    // All commands need EAL
    auto init_res = pm.init(opts.eal_args);
    if (!init_res.has_value()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::NotInitialized, init_res.error()});
    }

    if (opts.command == "init") {
        std::cout << "DPDK EAL initialized. Ports available: "
                  << pm.port_count() << "\n";
        return Result<void, SLError>::ok();
    }

    if (opts.command == "start") {
        auto cfg_res = pm.configure_port(opts.port_id, opts.nb_rx, opts.nb_tx);
        if (!cfg_res.has_value()) {
            return Result<void, SLError>::error(
                SLError{SLErrorCode::IOError, cfg_res.error()});
        }

        auto start_res = pm.start(opts.port_id);
        if (!start_res.has_value()) {
            return Result<void, SLError>::error(
                SLError{SLErrorCode::IOError, start_res.error()});
        }

        std::cout << "Port " << opts.port_id << " started with "
                  << opts.nb_rx << " RX / " << opts.nb_tx << " TX queues\n";
        return Result<void, SLError>::ok();
    }

    if (opts.command == "stats") {
        auto st = pm.stats(opts.port_id);
        std::cout << "Port " << opts.port_id << " statistics:\n"
                  << "  RX packets: " << st.rx_packets << "\n"
                  << "  TX packets: " << st.tx_packets << "\n"
                  << "  RX bytes:   " << st.rx_bytes << "\n"
                  << "  TX bytes:   " << st.tx_bytes << "\n"
                  << "  RX errors:  " << st.rx_errors << "\n"
                  << "  TX errors:  " << st.tx_errors << "\n"
                  << "  RX missed:  " << st.rx_missed << "\n";
        return Result<void, SLError>::ok();
    }

    if (opts.command == "stop") {
        pm.stop(opts.port_id);
        std::cout << "Port " << opts.port_id << " stopped\n";
        return Result<void, SLError>::ok();
    }

    return Result<void, SLError>::error(
        SLError{SLErrorCode::InvalidArgument, "Unreachable"});
}

} // anonymous namespace

int main(int argc, char* argv[]) {
    Log::init("straylight-dpdk");

    auto opts_res = parse_args(argc, argv);
    if (!opts_res.has_value()) {
        std::cerr << "Error: " << opts_res.error() << "\n\n";
        print_usage(argv[0]);
        return 1;
    }

    auto result = run(opts_res.value());
    if (!result.has_value()) {
        SL_ERROR("{}", result.error().message());
        return static_cast<int>(result.error().code());
    }

    return 0;
}
