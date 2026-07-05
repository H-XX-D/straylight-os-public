// bin/xdp/main.cpp
// CLI tool: straylight-xdp <load|attach|detach|stats> [options]
#include "loader.h"
#include "maps.h"
#include "af_xdp.h"

#include <straylight/result.h>
#include <straylight/error.h>
#include <straylight/log.h>

#include <linux/bpf.h>   // before bpf/bpf.h for bpf_stats_type
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <net/if.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace straylight;
using namespace straylight::xdp;

namespace {

struct Options {
    std::string command;       // load | attach | detach | stats
    std::string bpf_path;     // path to .o BPF object
    std::string prog_name;    // BPF program name within the object
    std::string ifname;       // network interface
    uint32_t    xdp_flags = 0;
};

void print_usage(const char* argv0) {
    std::cerr << "Usage:\n"
              << "  " << argv0 << " load   --bpf <path> --prog <name>\n"
              << "  " << argv0 << " attach --bpf <path> --prog <name> --iface <ifname> [--skb]\n"
              << "  " << argv0 << " detach --iface <ifname> [--skb]\n"
              << "  " << argv0 << " stats  --iface <ifname> [--skb]\n";
}

Result<Options, std::string> parse_args(int argc, char* argv[]) {
    if (argc < 2) {
        return Result<Options, std::string>::error("No command specified");
    }

    Options opts;
    opts.command = argv[1];

    if (opts.command != "load" && opts.command != "attach" &&
        opts.command != "detach" && opts.command != "stats") {
        return Result<Options, std::string>::error("Unknown command: " + opts.command);
    }

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--bpf" && i + 1 < argc) {
            opts.bpf_path = argv[++i];
        } else if (arg == "--prog" && i + 1 < argc) {
            opts.prog_name = argv[++i];
        } else if (arg == "--iface" && i + 1 < argc) {
            opts.ifname = argv[++i];
        } else if (arg == "--skb") {
            opts.xdp_flags = 2; // XDP_FLAGS_SKB_MODE
        } else {
            return Result<Options, std::string>::error("Unknown option: " + arg);
        }
    }

    // Validate required arguments per command
    if (opts.command == "load") {
        if (opts.bpf_path.empty() || opts.prog_name.empty()) {
            return Result<Options, std::string>::error("load requires --bpf and --prog");
        }
    } else if (opts.command == "attach") {
        if (opts.bpf_path.empty() || opts.prog_name.empty() || opts.ifname.empty()) {
            return Result<Options, std::string>::error(
                "attach requires --bpf, --prog, and --iface");
        }
    } else if (opts.command == "detach" || opts.command == "stats") {
        if (opts.ifname.empty()) {
            return Result<Options, std::string>::error(
                opts.command + " requires --iface");
        }
    }

    return Result<Options, std::string>::ok(std::move(opts));
}

Result<void, SLError> cmd_load(const Options& opts) {
    Loader loader;
    auto res = loader.load(opts.bpf_path, opts.prog_name);
    if (!res.has_value()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError, res.error()});
    }
    std::cout << "Loaded BPF program '" << opts.prog_name
              << "' from " << opts.bpf_path
              << " (fd=" << loader.prog_fd() << ")\n";
    return Result<void, SLError>::ok();
}

Result<void, SLError> cmd_attach(const Options& opts) {
    Loader loader;
    auto load_res = loader.load(opts.bpf_path, opts.prog_name);
    if (!load_res.has_value()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError, load_res.error()});
    }

    auto attach_res = loader.attach(opts.ifname, opts.xdp_flags);
    if (!attach_res.has_value()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError, attach_res.error()});
    }

    std::cout << "Attached XDP program '" << opts.prog_name
              << "' to interface " << opts.ifname
              << " (ifindex=" << loader.ifindex() << ")\n";

    // Keep the program attached after this one-shot CLI closes the BPF object.
    // Detach is a separate explicit command.
    loader.release_attachment();
    return Result<void, SLError>::ok();
}

Result<void, SLError> cmd_detach(const Options& opts) {
    // Detach any XDP program from the interface by attaching fd=-1
    unsigned int ifindex = if_nametoindex(opts.ifname.c_str());
    if (ifindex == 0) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::NotFound,
                    "Interface '" + opts.ifname + "' not found"});
    }

    int err;
#if defined(LIBBPF_MAJOR_VERSION) && (LIBBPF_MAJOR_VERSION > 0 || LIBBPF_MINOR_VERSION >= 7)
    err = bpf_xdp_detach(static_cast<int>(ifindex), opts.xdp_flags, nullptr);
#else
    err = bpf_set_link_xdp_fd(static_cast<int>(ifindex), -1, opts.xdp_flags);
#endif
    if (err) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError,
                    std::string("Failed to detach XDP: ") + std::strerror(-err)});
    }

    std::cout << "Detached XDP from " << opts.ifname << "\n";
    return Result<void, SLError>::ok();
}

Result<void, SLError> cmd_stats(const Options& opts) {
    unsigned int ifindex = if_nametoindex(opts.ifname.c_str());
    if (ifindex == 0) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::NotFound,
                    "Interface '" + opts.ifname + "' not found"});
    }

    // Query XDP program attached to interface
    uint32_t prog_id = 0;
#if defined(LIBBPF_MAJOR_VERSION) && (LIBBPF_MAJOR_VERSION > 0 || LIBBPF_MINOR_VERSION >= 7)
    struct bpf_xdp_query_opts query_opts = {};
    query_opts.sz = sizeof(query_opts);
    int err = bpf_xdp_query(static_cast<int>(ifindex), opts.xdp_flags, &query_opts);
    if (err) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError,
                    std::string("Failed to query XDP stats: ") + std::strerror(-err)});
    }
    prog_id = query_opts.prog_id;
#else
    int err = bpf_get_link_xdp_id(static_cast<int>(ifindex), &prog_id, opts.xdp_flags);
    if (err) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError,
                    std::string("Failed to query XDP stats: ") + std::strerror(-err)});
    }
#endif

    std::cout << "XDP stats for " << opts.ifname << " (ifindex=" << ifindex << "):\n"
              << "  prog_id: " << prog_id << "\n";
    if (prog_id == 0) {
        std::cout << "  (no XDP program attached)\n";
    }

    return Result<void, SLError>::ok();
}

} // anonymous namespace

int main(int argc, char* argv[]) {
    Log::init("straylight-xdp");

    auto opts_res = parse_args(argc, argv);
    if (!opts_res.has_value()) {
        std::cerr << "Error: " << opts_res.error() << "\n\n";
        print_usage(argv[0]);
        return 1;
    }

    const auto& opts = opts_res.value();
    Result<void, SLError> result = Result<void, SLError>::ok();

    if (opts.command == "load") {
        result = cmd_load(opts);
    } else if (opts.command == "attach") {
        result = cmd_attach(opts);
    } else if (opts.command == "detach") {
        result = cmd_detach(opts);
    } else if (opts.command == "stats") {
        result = cmd_stats(opts);
    }

    if (!result.has_value()) {
        SL_ERROR("{}", result.error().message());
        return static_cast<int>(result.error().code());
    }

    return 0;
}
