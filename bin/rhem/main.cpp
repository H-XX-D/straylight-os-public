// bin/rhem/main.cpp
// straylight-rhem — Heterogeneous resource manager
// Usage: straylight-rhem <scan|allocate|migrate|status> [options]

#include "allocator.h"
#include "discovery.h"
#include "migration.h"
#include "policy.h"

#include <straylight/error.h>
#include <straylight/result.h>

#include <cstdlib>
#include <iostream>
#include <string>

using namespace straylight;
using namespace straylight::rhem;

static void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " <command> [options]\n\n"
              << "Commands:\n"
              << "  scan       Discover all compute devices\n"
              << "  allocate   --type <cpu|cuda|rocm|fpga|tpu> --mem <bytes> --compute <0-1>\n"
              << "  migrate    --src <id> --dst <id> --bytes <N>\n"
              << "  status     Show current allocations and device status\n"
              << "  policy     --type <round-robin|least-loaded|affinity|power-efficient>\n"
              << "             --mem <bytes>  Select device by policy\n";
}

static std::string find_arg(int argc, char* argv[], const std::string& flag) {
    for (int i = 0; i < argc - 1; ++i) {
        if (argv[i] == flag) return argv[i + 1];
    }
    return "";
}

static Result<void, SLError> cmd_scan() {
    DeviceDiscovery disco;
    auto result = disco.scan();
    if (!result.has_value()) {
        return Result<void, SLError>::error(
            {SLErrorCode::HardwareFault, result.error()});
    }

    const auto& devices = result.value();
    std::cout << "Discovered " << devices.size() << " device(s):\n\n";

    for (const auto& dev : devices) {
        std::cout << "  [" << dev.id << "] " << device_type_str(dev.type)
                  << ": " << dev.name << "\n"
                  << "      Memory: " << (dev.memory_bytes / (1024 * 1024)) << " MB\n"
                  << "      Compute: " << dev.compute_tflops << " TFLOPS\n"
                  << "      Available: " << (dev.available ? "yes" : "no") << "\n\n";
    }

    return Result<void, SLError>::ok();
}

static Result<void, SLError> cmd_allocate(int argc, char* argv[]) {
    std::string type_str = find_arg(argc, argv, "--type");
    std::string mem_str = find_arg(argc, argv, "--mem");
    std::string compute_str = find_arg(argc, argv, "--compute");

    if (type_str.empty() || mem_str.empty()) {
        return Result<void, SLError>::error(
            {SLErrorCode::InvalidArgument, "allocate requires --type and --mem"});
    }

    DeviceType preferred = device_type_from_str(type_str);
    size_t mem = std::stoull(mem_str);
    float compute = compute_str.empty() ? 0.5f : std::stof(compute_str);

    // Scan devices first
    DeviceDiscovery disco;
    auto scan_result = disco.scan();
    if (!scan_result.has_value()) {
        return Result<void, SLError>::error(
            {SLErrorCode::HardwareFault, scan_result.error()});
    }

    ResourceAllocator allocator(std::move(scan_result).value());
    auto alloc_result = allocator.allocate(preferred, mem, compute);
    if (!alloc_result.has_value()) {
        return Result<void, SLError>::error(
            {SLErrorCode::NotFound, alloc_result.error()});
    }

    const auto& alloc = alloc_result.value();
    std::cout << "Allocation successful:\n"
              << "  Lease ID: " << alloc.lease_id << "\n"
              << "  Device ID: " << alloc.device_id << "\n"
              << "  Memory: " << alloc.memory_bytes << " bytes\n"
              << "  Compute: " << alloc.compute_fraction << "\n";

    return Result<void, SLError>::ok();
}

static Result<void, SLError> cmd_migrate(int argc, char* argv[]) {
    std::string src_str = find_arg(argc, argv, "--src");
    std::string dst_str = find_arg(argc, argv, "--dst");
    std::string bytes_str = find_arg(argc, argv, "--bytes");

    if (src_str.empty() || dst_str.empty() || bytes_str.empty()) {
        return Result<void, SLError>::error(
            {SLErrorCode::InvalidArgument, "migrate requires --src, --dst, and --bytes"});
    }

    uint32_t src = static_cast<uint32_t>(std::stoul(src_str));
    uint32_t dst = static_cast<uint32_t>(std::stoul(dst_str));
    size_t bytes = std::stoull(bytes_str);

    // Scan devices
    DeviceDiscovery disco;
    auto scan_result = disco.scan();
    if (!scan_result.has_value()) {
        return Result<void, SLError>::error(
            {SLErrorCode::HardwareFault, scan_result.error()});
    }

    Migrator migrator(std::move(scan_result).value());
    auto plan_result = migrator.plan(src, dst, bytes);
    if (!plan_result.has_value()) {
        return Result<void, SLError>::error(
            {SLErrorCode::Internal, plan_result.error()});
    }

    const auto& plan = plan_result.value();
    std::cout << "Migration plan:\n"
              << "  Source: device " << plan.src_device << "\n"
              << "  Destination: device " << plan.dst_device << "\n"
              << "  Tensor: " << plan.tensor_bytes << " bytes\n"
              << "  Estimated time: " << plan.estimated_time_ms << " ms\n";

    auto exec_result = migrator.execute(plan);
    if (!exec_result.has_value()) {
        return Result<void, SLError>::error(
            {SLErrorCode::Internal, exec_result.error()});
    }

    std::cout << "Migration executed successfully.\n";
    return Result<void, SLError>::ok();
}

static Result<void, SLError> cmd_status() {
    DeviceDiscovery disco;
    auto scan_result = disco.scan();
    if (!scan_result.has_value()) {
        return Result<void, SLError>::error(
            {SLErrorCode::HardwareFault, scan_result.error()});
    }

    const auto& devices = scan_result.value();
    ResourceAllocator allocator(devices);

    std::cout << "Device status (" << devices.size() << " device(s)):\n";
    for (const auto& dev : devices) {
        std::cout << "  [" << dev.id << "] " << device_type_str(dev.type)
                  << ": " << dev.name
                  << " (" << (dev.memory_bytes / (1024 * 1024)) << " MB, "
                  << dev.compute_tflops << " TFLOPS)"
                  << (dev.available ? "" : " [UNAVAILABLE]") << "\n";
    }

    auto active = allocator.active();
    if (active.empty()) {
        std::cout << "\nNo active allocations.\n";
    } else {
        std::cout << "\nActive allocations:\n";
        for (const auto& a : active) {
            std::cout << "  Lease " << a.lease_id << ": device " << a.device_id
                      << ", " << a.memory_bytes << " bytes, "
                      << a.compute_fraction << " compute\n";
        }
    }

    return Result<void, SLError>::ok();
}

static Result<void, SLError> cmd_policy(int argc, char* argv[]) {
    std::string type_str = find_arg(argc, argv, "--type");
    std::string mem_str = find_arg(argc, argv, "--mem");

    if (type_str.empty()) {
        return Result<void, SLError>::error(
            {SLErrorCode::InvalidArgument, "policy requires --type"});
    }

    PolicyType policy = policy_type_from_str(type_str);
    size_t mem = mem_str.empty() ? 0 : std::stoull(mem_str);

    DeviceDiscovery disco;
    auto scan_result = disco.scan();
    if (!scan_result.has_value()) {
        return Result<void, SLError>::error(
            {SLErrorCode::HardwareFault, scan_result.error()});
    }

    PlacementPolicy pp;
    auto sel = pp.select_device(scan_result.value(), mem, policy);
    if (!sel.has_value()) {
        return Result<void, SLError>::error(
            {SLErrorCode::NotFound, sel.error()});
    }

    std::cout << "Policy " << policy_type_str(policy)
              << " selected device " << sel.value() << "\n";

    return Result<void, SLError>::ok();
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string cmd = argv[1];
    Result<void, SLError> result = Result<void, SLError>::ok();

    if (cmd == "scan") {
        result = cmd_scan();
    } else if (cmd == "allocate") {
        result = cmd_allocate(argc, argv);
    } else if (cmd == "migrate") {
        result = cmd_migrate(argc, argv);
    } else if (cmd == "status") {
        result = cmd_status();
    } else if (cmd == "policy") {
        result = cmd_policy(argc, argv);
    } else if (cmd == "--help" || cmd == "-h") {
        print_usage(argv[0]);
        return 0;
    } else {
        std::cerr << "Unknown command: " << cmd << "\n";
        print_usage(argv[0]);
        return 1;
    }

    if (!result.has_value()) {
        std::cerr << "Error [" << static_cast<int>(result.error().code())
                  << "]: " << result.error().message() << "\n";
        return static_cast<int>(result.error().code());
    }

    return 0;
}
