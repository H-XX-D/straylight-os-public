// bin/pmem/main.cpp
// straylight-pmem — Persistent memory manager
// Usage: straylight-pmem <map|alloc|checkpoint|status> [options]

#include "allocator.h"
#include "checkpoint.h"
#include "dax.h"
#include "log.h"

#include <straylight/error.h>
#include <straylight/result.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

using namespace straylight;
using namespace straylight::pmem;

static void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " <command> [options]\n\n"
              << "Commands:\n"
              << "  map         --path <dax-path> --size <bytes>\n"
              << "  alloc       --path <dax-path> --size <region-bytes> --alloc <N>\n"
              << "  checkpoint  --dir <dir> <save|load|list|remove> [--name <n>] [--data <hex>]\n"
              << "  status      --path <dax-path>\n";
}

static std::string find_arg(int argc, char* argv[], const std::string& flag) {
    for (int i = 0; i < argc - 1; ++i) {
        if (argv[i] == flag) return argv[i + 1];
    }
    return "";
}

static Result<void, SLError> cmd_map(int argc, char* argv[]) {
    std::string path = find_arg(argc, argv, "--path");
    std::string size_str = find_arg(argc, argv, "--size");

    if (path.empty()) {
        return Result<void, SLError>::error(
            {SLErrorCode::InvalidArgument, "map requires --path"});
    }

    size_t size = size_str.empty() ? (1024 * 1024) : std::stoull(size_str);

    DaxManager dax;
    bool pmem = dax.is_pmem(path);
    std::cout << "Device: " << path << "\n";
    std::cout << "Is PMEM: " << (pmem ? "yes" : "no (file-backed simulation)") << "\n";

    auto map_result = dax.map_region(path, size);
    if (!map_result.has_value()) {
        return Result<void, SLError>::error({SLErrorCode::IOError, map_result.error()});
    }

    void* addr = map_result.value();
    std::cout << "Mapped " << size << " bytes at " << addr << "\n";

    // Write a test pattern and flush.
    std::memset(addr, 0xAA, std::min(size, static_cast<size_t>(4096)));
    auto flush_result = dax.flush(addr, std::min(size, static_cast<size_t>(4096)));
    if (!flush_result.has_value()) {
        std::cerr << "Warning: flush failed: " << flush_result.error() << "\n";
    } else {
        std::cout << "Flushed test pattern to persistence\n";
    }

    auto unmap_result = dax.unmap(addr, size);
    if (!unmap_result.has_value()) {
        return Result<void, SLError>::error({SLErrorCode::IOError, unmap_result.error()});
    }

    std::cout << "Unmapped successfully\n";
    return Result<void, SLError>::ok();
}

static Result<void, SLError> cmd_alloc(int argc, char* argv[]) {
    std::string path = find_arg(argc, argv, "--path");
    std::string size_str = find_arg(argc, argv, "--size");
    std::string alloc_str = find_arg(argc, argv, "--alloc");

    if (path.empty()) {
        return Result<void, SLError>::error(
            {SLErrorCode::InvalidArgument, "alloc requires --path"});
    }

    size_t region_size = size_str.empty() ? (1024 * 1024) : std::stoull(size_str);
    size_t alloc_size = alloc_str.empty() ? 256 : std::stoull(alloc_str);

    DaxManager dax;
    auto map_result = dax.map_region(path, region_size);
    if (!map_result.has_value()) {
        return Result<void, SLError>::error({SLErrorCode::IOError, map_result.error()});
    }

    void* base = map_result.value();
    PmemAllocator allocator;
    auto init_result = allocator.init(base, region_size);
    if (!init_result.has_value()) {
        dax.unmap(base, region_size);
        return Result<void, SLError>::error({SLErrorCode::Internal, init_result.error()});
    }

    std::cout << "Allocator initialized: " << region_size << " bytes\n";

    // Perform a test allocation.
    auto alloc_result = allocator.alloc(alloc_size);
    if (!alloc_result.has_value()) {
        dax.unmap(base, region_size);
        return Result<void, SLError>::error({SLErrorCode::Internal, alloc_result.error()});
    }

    std::cout << "Allocated " << alloc_size << " bytes at " << alloc_result.value() << "\n";
    std::cout << "Used: " << allocator.used() << " Available: " << allocator.available() << "\n";

    // Free and verify.
    auto free_result = allocator.free(alloc_result.value());
    if (!free_result.has_value()) {
        std::cerr << "Warning: free failed: " << free_result.error() << "\n";
    } else {
        std::cout << "Freed. Used: " << allocator.used()
                  << " Available: " << allocator.available() << "\n";
    }

    dax.unmap(base, region_size);
    return Result<void, SLError>::ok();
}

static Result<void, SLError> cmd_checkpoint(int argc, char* argv[]) {
    std::string dir = find_arg(argc, argv, "--dir");
    std::string name = find_arg(argc, argv, "--name");
    std::string data_str = find_arg(argc, argv, "--data");

    if (dir.empty()) dir = "/tmp/straylight-checkpoints";

    CheckpointManager mgr(dir);

    // Find the sub-command after "checkpoint".
    std::string subcmd;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "save" || arg == "load" || arg == "list" || arg == "remove") {
            subcmd = arg;
            break;
        }
    }

    if (subcmd == "save") {
        if (name.empty()) {
            return Result<void, SLError>::error(
                {SLErrorCode::InvalidArgument, "save requires --name"});
        }
        // Use provided data or generate test data.
        std::vector<uint8_t> data;
        if (!data_str.empty()) {
            data.assign(data_str.begin(), data_str.end());
        } else {
            // Generate 1KB of test tensor data.
            data.resize(1024);
            for (size_t i = 0; i < data.size(); ++i) {
                data[i] = static_cast<uint8_t>(i & 0xFF);
            }
        }
        auto r = mgr.save(name, data.data(), data.size());
        if (!r.has_value()) {
            return Result<void, SLError>::error({SLErrorCode::IOError, r.error()});
        }
        std::cout << "Saved checkpoint '" << name << "' (" << data.size() << " bytes)\n";

    } else if (subcmd == "load") {
        if (name.empty()) {
            return Result<void, SLError>::error(
                {SLErrorCode::InvalidArgument, "load requires --name"});
        }
        auto r = mgr.load(name);
        if (!r.has_value()) {
            return Result<void, SLError>::error({SLErrorCode::NotFound, r.error()});
        }
        std::cout << "Loaded checkpoint '" << name << "' (" << r.value().size() << " bytes)\n";

    } else if (subcmd == "list") {
        auto r = mgr.list();
        if (!r.has_value()) {
            return Result<void, SLError>::error({SLErrorCode::IOError, r.error()});
        }
        std::cout << "Checkpoints in " << dir << ":\n";
        for (const auto& n : r.value()) {
            std::cout << "  " << n << "\n";
        }
        if (r.value().empty()) {
            std::cout << "  (none)\n";
        }

    } else if (subcmd == "remove") {
        if (name.empty()) {
            return Result<void, SLError>::error(
                {SLErrorCode::InvalidArgument, "remove requires --name"});
        }
        auto r = mgr.remove(name);
        if (!r.has_value()) {
            return Result<void, SLError>::error({SLErrorCode::NotFound, r.error()});
        }
        std::cout << "Removed checkpoint '" << name << "'\n";

    } else {
        return Result<void, SLError>::error(
            {SLErrorCode::InvalidArgument,
             "checkpoint requires sub-command: save|load|list|remove"});
    }

    return Result<void, SLError>::ok();
}

static Result<void, SLError> cmd_status(int argc, char* argv[]) {
    std::string path = find_arg(argc, argv, "--path");
    if (path.empty()) {
        return Result<void, SLError>::error(
            {SLErrorCode::InvalidArgument, "status requires --path"});
    }

    DaxManager dax;
    std::cout << "Device: " << path << "\n";
    std::cout << "Is PMEM: " << (dax.is_pmem(path) ? "yes" : "no") << "\n";

    // Try mapping a small region to test accessibility.
    auto r = dax.map_region(path, 4096);
    if (!r.has_value()) {
        std::cout << "Status: UNAVAILABLE (" << r.error() << ")\n";
    } else {
        std::cout << "Status: AVAILABLE\n";
        std::cout << "Mapped test region at " << r.value() << "\n";
        dax.unmap(r.value(), 4096);
    }

    return Result<void, SLError>::ok();
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string cmd = argv[1];
    Result<void, SLError> result = Result<void, SLError>::ok();

    if (cmd == "map") {
        result = cmd_map(argc, argv);
    } else if (cmd == "alloc") {
        result = cmd_alloc(argc, argv);
    } else if (cmd == "checkpoint") {
        result = cmd_checkpoint(argc, argv);
    } else if (cmd == "status") {
        result = cmd_status(argc, argv);
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
