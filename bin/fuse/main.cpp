// bin/fuse/main.cpp
// straylight-fuse — Tensor compression FUSE filesystem daemon entry point.
// Drives FuseDaemon through DaemonBase::run().

#include "fuse_daemon.h"

#include <straylight/config.h>
#include <straylight/log.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

static void print_usage(const char* prog) {
    std::cerr
        << "Usage: " << prog << " [options]\n\n"
        << "Options:\n"
        << "  --config <path>   Config file (default: /etc/straylight/fuse.conf)\n"
        << "  --mount  <path>   Override fuse.mountpoint\n"
        << "  --store  <path>   Override fuse.store_dir\n"
        << "  --help            Show this message\n\n"
        << "Config file is JSON with keys:\n"
        << "  fuse.mountpoint   Mount directory (default: /var/lib/straylight/tensors)\n"
        << "  fuse.store_dir    Compressed tensor storage dir\n"
        << "  fuse.cache_mb     LRU cache size in MiB (default: 512)\n";
}

int main(int argc, char* argv[]) {
    std::string config_path    = "/etc/straylight/fuse.conf";
    std::string mount_override;
    std::string store_override;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if ((arg == "--config") && i + 1 < argc) {
            config_path = argv[++i];
        } else if ((arg == "--mount") && i + 1 < argc) {
            mount_override = argv[++i];
        } else if ((arg == "--store") && i + 1 < argc) {
            store_override = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        }
    }

    straylight::Log::init("straylight-fuse");

    // Load config.  If the file is missing, create an in-memory default.
    auto cfg_result = straylight::Config::load(config_path);
    if (!cfg_result.has_value()) {
        // Write a minimal default config to a temp file and load it.
        namespace fs = std::filesystem;
        const fs::path tmp = fs::temp_directory_path() / "sl_fuse_defaults.json";
        {
            std::ofstream f(tmp);
            f << "{\n"
              << "  \"fuse\": {\n"
              << "    \"mountpoint\": \""
              << (mount_override.empty() ? "/var/lib/straylight/tensors"
                                        : mount_override)
              << "\",\n"
              << "    \"store_dir\": \""
              << (store_override.empty() ? "/var/lib/straylight/tensor-store"
                                        : store_override)
              << "\",\n"
              << "    \"cache_mb\": 512\n"
              << "  }\n"
              << "}\n";
        }
        cfg_result = straylight::Config::load(tmp);
        fs::remove(tmp);
    }

    if (!cfg_result.has_value()) {
        std::cerr << "Cannot load configuration: " << cfg_result.error() << "\n";
        return 1;
    }

    straylight::fuse::FuseDaemon daemon;
    return daemon.run(cfg_result.value());
}
