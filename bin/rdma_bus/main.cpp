// bin/rdma_bus/main.cpp
// CLI tool: straylight-rdma-bus <connect|send|recv|status> [options]
#include "verbs.h"
#include "memory_region.h"
#include "queue_pair.h"
#include "tensor_rdma.h"

#include <straylight/result.h>
#include <straylight/error.h>
#include <straylight/log.h>

#include <infiniband/verbs.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace straylight;
using namespace straylight::rdma;

namespace {

struct Options {
    std::string command;       // connect | send | recv | status
    std::string device;        // RDMA device name (e.g. "mlx5_0")
    std::string remote_addr;   // Remote IP/hostname
    uint16_t    remote_port = 18515;
    std::string tensor_file;   // Path to tensor file for send
    std::string output_file;   // Path to write received tensor
    uint64_t    remote_mr_addr = 0;
    uint32_t    remote_mr_rkey = 0;
    size_t      tensor_size    = 0;
};

void print_usage(const char* argv0) {
    std::cerr << "Usage:\n"
              << "  " << argv0 << " connect --device <dev> --remote <addr> [--port <p>]\n"
              << "  " << argv0 << " send    --device <dev> --remote <addr> --file <path>"
              << " --raddr <hex> --rkey <hex>\n"
              << "  " << argv0 << " recv    --device <dev> --remote <addr> --output <path>"
              << " --size <bytes>\n"
              << "  " << argv0 << " status  [--device <dev>]\n";
}

Result<Options, std::string> parse_args(int argc, char* argv[]) {
    if (argc < 2) {
        return Result<Options, std::string>::error("No command specified");
    }

    Options opts;
    opts.command = argv[1];

    if (opts.command != "connect" && opts.command != "send" &&
        opts.command != "recv" && opts.command != "status") {
        return Result<Options, std::string>::error("Unknown command: " + opts.command);
    }

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--device" && i + 1 < argc) {
            opts.device = argv[++i];
        } else if (arg == "--remote" && i + 1 < argc) {
            opts.remote_addr = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            opts.remote_port = static_cast<uint16_t>(std::stoul(argv[++i]));
        } else if (arg == "--file" && i + 1 < argc) {
            opts.tensor_file = argv[++i];
        } else if (arg == "--output" && i + 1 < argc) {
            opts.output_file = argv[++i];
        } else if (arg == "--raddr" && i + 1 < argc) {
            opts.remote_mr_addr = std::stoull(argv[++i], nullptr, 16);
        } else if (arg == "--rkey" && i + 1 < argc) {
            opts.remote_mr_rkey = static_cast<uint32_t>(std::stoul(argv[++i], nullptr, 16));
        } else if (arg == "--size" && i + 1 < argc) {
            opts.tensor_size = std::stoull(argv[++i]);
        } else {
            return Result<Options, std::string>::error("Unknown option: " + arg);
        }
    }

    // Validate per command
    if (opts.command == "connect") {
        if (opts.remote_addr.empty()) {
            return Result<Options, std::string>::error("connect requires --remote");
        }
    } else if (opts.command == "send") {
        if (opts.remote_addr.empty() || opts.tensor_file.empty()) {
            return Result<Options, std::string>::error(
                "send requires --remote, --file, --raddr, --rkey");
        }
    } else if (opts.command == "recv") {
        if (opts.remote_addr.empty() || opts.output_file.empty() || opts.tensor_size == 0) {
            return Result<Options, std::string>::error(
                "recv requires --remote, --output, --size");
        }
    }
    // status has no required args

    return Result<Options, std::string>::ok(std::move(opts));
}

Result<void, SLError> cmd_status(const Options& opts) {
    // List available RDMA devices
    int num_devices = 0;
    struct ibv_device** dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list || num_devices == 0) {
        if (dev_list) ibv_free_device_list(dev_list);
        std::cout << "No RDMA devices found\n";
        return Result<void, SLError>::ok();
    }

    std::cout << "RDMA devices (" << num_devices << "):\n";
    for (int i = 0; i < num_devices; ++i) {
        const char* name = ibv_get_device_name(dev_list[i]);
        uint64_t guid = ibv_get_device_guid(dev_list[i]);

        std::cout << "  [" << i << "] " << name
                  << "  GUID=" << std::hex << guid << std::dec << "\n";

        // If a specific device was requested, show detailed info
        if (!opts.device.empty() && opts.device == name) {
            struct ibv_context* ctx = ibv_open_device(dev_list[i]);
            if (ctx) {
                struct ibv_device_attr attr;
                if (ibv_query_device(ctx, &attr) == 0) {
                    std::cout << "    Max QPs:        " << attr.max_qp << "\n"
                              << "    Max QP WRs:     " << attr.max_qp_wr << "\n"
                              << "    Max CQs:        " << attr.max_cq << "\n"
                              << "    Max CQ entries: " << attr.max_cqe << "\n"
                              << "    Max MRs:        " << attr.max_mr << "\n"
                              << "    Max PDs:        " << attr.max_pd << "\n"
                              << "    Phys ports:     " << static_cast<int>(attr.phys_port_cnt) << "\n";
                }
                ibv_close_device(ctx);
            }
        }
    }

    ibv_free_device_list(dev_list);
    return Result<void, SLError>::ok();
}

Result<void, SLError> cmd_connect(const Options& opts) {
    VerbsContext verbs;
    auto open_res = verbs.open(opts.device);
    if (!open_res.has_value()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError, open_res.error()});
    }

    auto pd_res = verbs.create_pd();
    if (!pd_res.has_value()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError, pd_res.error()});
    }

    auto cq_res = verbs.create_cq(256);
    if (!cq_res.has_value()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError, cq_res.error()});
    }

    MemoryRegionManager mr_mgr(verbs);
    QueuePairManager qp_mgr(verbs);
    TensorRdma tensor(verbs, qp_mgr, mr_mgr);

    auto conn_res = tensor.connect(opts.remote_addr, opts.remote_port);
    if (!conn_res.has_value()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError, conn_res.error()});
    }

    std::cout << "Connected to " << opts.remote_addr << ":" << opts.remote_port
              << " (local QP=" << tensor.local_qp_num() << ")\n";

    return Result<void, SLError>::ok();
}

Result<void, SLError> cmd_send(const Options& opts) {
    // Read tensor from file
    std::ifstream infile(opts.tensor_file, std::ios::binary | std::ios::ate);
    if (!infile) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError, "Cannot open " + opts.tensor_file});
    }
    auto file_size = static_cast<size_t>(infile.tellg());
    infile.seekg(0);

    std::vector<uint8_t> tensor_data(file_size);
    infile.read(reinterpret_cast<char*>(tensor_data.data()),
                static_cast<std::streamsize>(file_size));
    infile.close();

    // Set up RDMA stack
    VerbsContext verbs;
    auto open_res = verbs.open(opts.device);
    if (!open_res.has_value()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError, open_res.error()});
    }
    auto pd_res = verbs.create_pd();
    if (!pd_res.has_value()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError, pd_res.error()});
    }
    auto cq_res = verbs.create_cq(256);
    if (!cq_res.has_value()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError, cq_res.error()});
    }

    MemoryRegionManager mr_mgr(verbs);
    QueuePairManager qp_mgr(verbs);
    TensorRdma tensor_rdma(verbs, qp_mgr, mr_mgr);

    // Register memory region
    auto mr_res = mr_mgr.register_region(tensor_data.data(), tensor_data.size());
    if (!mr_res.has_value()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError, mr_res.error()});
    }

    auto conn_res = tensor_rdma.connect(opts.remote_addr, opts.remote_port);
    if (!conn_res.has_value()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError, conn_res.error()});
    }

    auto write_res = tensor_rdma.write_tensor(
        mr_res.value(), opts.remote_mr_addr, opts.remote_mr_rkey, file_size);
    if (!write_res.has_value()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError, write_res.error()});
    }

    std::cout << "Sent " << file_size << " bytes via RDMA write\n";
    return Result<void, SLError>::ok();
}

Result<void, SLError> cmd_recv(const Options& opts) {
    VerbsContext verbs;
    auto open_res = verbs.open(opts.device);
    if (!open_res.has_value()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError, open_res.error()});
    }
    auto pd_res = verbs.create_pd();
    if (!pd_res.has_value()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError, pd_res.error()});
    }
    auto cq_res = verbs.create_cq(256);
    if (!cq_res.has_value()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError, cq_res.error()});
    }

    MemoryRegionManager mr_mgr(verbs);
    QueuePairManager qp_mgr(verbs);
    TensorRdma tensor_rdma(verbs, qp_mgr, mr_mgr);

    // Allocate receive buffer and register it
    std::vector<uint8_t> recv_buf(opts.tensor_size);
    auto mr_res = mr_mgr.register_region(recv_buf.data(), recv_buf.size());
    if (!mr_res.has_value()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError, mr_res.error()});
    }

    auto conn_res = tensor_rdma.connect(opts.remote_addr, opts.remote_port);
    if (!conn_res.has_value()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError, conn_res.error()});
    }

    // RDMA read from remote
    auto read_res = tensor_rdma.read_tensor(
        opts.remote_mr_addr, opts.remote_mr_rkey,
        mr_res.value(), opts.tensor_size);
    if (!read_res.has_value()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError, read_res.error()});
    }

    // Write to file
    std::ofstream outfile(opts.output_file, std::ios::binary);
    if (!outfile) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::IOError, "Cannot open " + opts.output_file});
    }
    outfile.write(reinterpret_cast<const char*>(recv_buf.data()),
                  static_cast<std::streamsize>(opts.tensor_size));
    outfile.close();

    std::cout << "Received " << opts.tensor_size << " bytes via RDMA read -> "
              << opts.output_file << "\n";
    return Result<void, SLError>::ok();
}

} // anonymous namespace

int main(int argc, char* argv[]) {
    Log::init("straylight-rdma-bus");

    auto opts_res = parse_args(argc, argv);
    if (!opts_res.has_value()) {
        std::cerr << "Error: " << opts_res.error() << "\n\n";
        print_usage(argv[0]);
        return 1;
    }

    const auto& opts = opts_res.value();
    Result<void, SLError> result = Result<void, SLError>::ok();

    if (opts.command == "connect") {
        result = cmd_connect(opts);
    } else if (opts.command == "send") {
        result = cmd_send(opts);
    } else if (opts.command == "recv") {
        result = cmd_recv(opts);
    } else if (opts.command == "status") {
        result = cmd_status(opts);
    }

    if (!result.has_value()) {
        SL_ERROR("{}", result.error().message());
        return static_cast<int>(result.error().code());
    }

    return 0;
}
