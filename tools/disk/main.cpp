// tools/disk/main.cpp
// straylight-disk — Disk and partition manager for StrayLight OS.
#include "disk_manager.h"

#include <straylight/log.h>

#include <iomanip>
#include <iostream>
#include <string>

using straylight::DiskManager;

static void print_usage() {
    std::cerr
        << "straylight-disk — Disk & partition manager\n\n"
        << "Usage:\n"
        << "  straylight-disk list                     List block devices\n"
        << "  straylight-disk info <device>             Device details\n"
        << "  straylight-disk mount <device> <path>     Mount a device\n"
        << "  straylight-disk unmount <path>            Unmount a device\n"
        << "  straylight-disk format <device> <fstype>  Format a device\n"
        << "  straylight-disk resize <device> <size>    Resize filesystem\n"
        << "  straylight-disk smart <device>            SMART health data\n"
        << "  straylight-disk encrypt <device>          LUKS encrypt a device\n"
        << "  straylight-disk benchmark <device>        Disk performance test\n"
        << "  straylight-disk eject <device>            Safe eject USB device\n"
        << "\nFilesystem types: ext4, btrfs, xfs, ntfs, fat32, exfat, swap\n"
        << "\nExamples:\n"
        << "  straylight-disk list\n"
        << "  straylight-disk mount /dev/sdb1 /mnt/usb\n"
        << "  straylight-disk format /dev/sdb1 ext4\n"
        << "  straylight-disk smart /dev/sda\n";
}

/// Format bytes into human-readable string.
static std::string fmt_bytes(uint64_t bytes) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1);
    if (bytes >= 1024ULL * 1024 * 1024 * 1024) {
        oss << static_cast<double>(bytes) / (1024.0 * 1024 * 1024 * 1024) << " TiB";
    } else if (bytes >= 1024ULL * 1024 * 1024) {
        oss << static_cast<double>(bytes) / (1024.0 * 1024 * 1024) << " GiB";
    } else if (bytes >= 1024ULL * 1024) {
        oss << static_cast<double>(bytes) / (1024.0 * 1024) << " MiB";
    } else {
        oss << bytes << " B";
    }
    return oss.str();
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string cmd = argv[1];
    if (cmd == "--help" || cmd == "-h") {
        print_usage();
        return 0;
    }

    DiskManager mgr;

    if (cmd == "list") {
        auto r = mgr.list_devices();
        if (!r.has_value()) {
            std::cerr << "Error: " << r.error().message() << "\n";
            return 1;
        }

        const auto& devices = r.value();
        if (devices.empty()) {
            std::cout << "No block devices found.\n";
            return 0;
        }

        std::cout << std::left
                  << std::setw(12) << "NAME"
                  << std::setw(10) << "SIZE"
                  << std::setw(8) << "TYPE"
                  << std::setw(8) << "FSTYPE"
                  << std::setw(8) << "TRAN"
                  << std::setw(20) << "MOUNTPOINT"
                  << "MODEL\n";
        std::cout << std::string(80, '-') << "\n";

        for (const auto& dev : devices) {
            std::cout << std::setw(12) << dev.name
                      << std::setw(10) << fmt_bytes(dev.size_bytes)
                      << std::setw(8) << dev.type
                      << std::setw(8) << (dev.fstype.empty() ? "-" : dev.fstype)
                      << std::setw(8) << (dev.transport.empty() ? "-" : dev.transport)
                      << std::setw(20) << (dev.mountpoint.empty() ? "-" : dev.mountpoint)
                      << dev.model << "\n";

            for (const auto& child : dev.children) {
                std::cout << "  " << std::setw(10) << child.name
                          << std::setw(10) << fmt_bytes(child.size_bytes)
                          << std::setw(8) << child.type
                          << std::setw(8) << (child.fstype.empty() ? "-" : child.fstype)
                          << std::setw(8) << "-"
                          << std::setw(20) << (child.mountpoint.empty() ? "-" : child.mountpoint)
                          << "\n";
            }
        }

    } else if (cmd == "info") {
        if (argc < 3) {
            std::cerr << "Usage: straylight-disk info <device>\n";
            return 1;
        }
        auto r = mgr.device_info(argv[2]);
        if (!r.has_value()) {
            std::cerr << "Error: " << r.error().message() << "\n";
            return 1;
        }
        const auto& dev = r.value();

        std::cout << "Device:      " << dev.path << "\n";
        std::cout << "Name:        " << dev.name << "\n";
        std::cout << "Type:        " << dev.type << "\n";
        std::cout << "Size:        " << fmt_bytes(dev.size_bytes) << "\n";
        if (!dev.model.empty())      std::cout << "Model:       " << dev.model << "\n";
        if (!dev.serial.empty())     std::cout << "Serial:      " << dev.serial << "\n";
        if (!dev.fstype.empty())     std::cout << "Filesystem:  " << dev.fstype << "\n";
        if (!dev.label.empty())      std::cout << "Label:       " << dev.label << "\n";
        if (!dev.uuid.empty())       std::cout << "UUID:        " << dev.uuid << "\n";
        if (!dev.mountpoint.empty()) std::cout << "Mountpoint:  " << dev.mountpoint << "\n";
        if (!dev.transport.empty())  std::cout << "Transport:   " << dev.transport << "\n";
        std::cout << "Removable:   " << (dev.removable ? "yes" : "no") << "\n";
        std::cout << "Read-only:   " << (dev.readonly ? "yes" : "no") << "\n";

        // Show filesystem usage if mounted.
        if (!dev.mountpoint.empty()) {
            auto usage = mgr.fs_usage(dev.mountpoint);
            if (usage.has_value()) {
                const auto& u = usage.value();
                std::cout << "\nFilesystem usage:\n";
                std::cout << "  Total:     " << fmt_bytes(u.total_bytes) << "\n";
                std::cout << "  Used:      " << fmt_bytes(u.used_bytes)
                          << " (" << std::fixed << std::setprecision(1) << u.usage_percent << "%)\n";
                std::cout << "  Available: " << fmt_bytes(u.available_bytes) << "\n";
                std::cout << "  Inodes:    " << u.used_inodes << " / " << u.total_inodes << "\n";
            }
        }

    } else if (cmd == "mount") {
        if (argc < 4) {
            std::cerr << "Usage: straylight-disk mount <device> <path>\n";
            return 1;
        }
        auto r = mgr.mount(argv[2], argv[3]);
        if (!r.has_value()) {
            std::cerr << "Error: " << r.error().message() << "\n";
            return 1;
        }
        std::cout << "Mounted " << argv[2] << " at " << argv[3] << "\n";

    } else if (cmd == "unmount") {
        if (argc < 3) {
            std::cerr << "Usage: straylight-disk unmount <path>\n";
            return 1;
        }
        auto r = mgr.unmount(argv[2]);
        if (!r.has_value()) {
            std::cerr << "Error: " << r.error().message() << "\n";
            return 1;
        }
        std::cout << "Unmounted " << argv[2] << "\n";

    } else if (cmd == "format") {
        if (argc < 4) {
            std::cerr << "Usage: straylight-disk format <device> <fstype>\n";
            return 1;
        }
        std::string label = "";
        if (argc >= 5) label = argv[4];
        auto r = mgr.format(argv[2], argv[3], label);
        if (!r.has_value()) {
            std::cerr << "Error: " << r.error().message() << "\n";
            return 1;
        }
        std::cout << "Formatted " << argv[2] << " as " << argv[3] << "\n";

    } else if (cmd == "resize") {
        if (argc < 4) {
            std::cerr << "Usage: straylight-disk resize <device> <size>\n";
            return 1;
        }
        auto r = mgr.resize(argv[2], argv[3]);
        if (!r.has_value()) {
            std::cerr << "Error: " << r.error().message() << "\n";
            return 1;
        }
        std::cout << "Resized " << argv[2] << " to " << argv[3] << "\n";

    } else if (cmd == "smart") {
        if (argc < 3) {
            std::cerr << "Usage: straylight-disk smart <device>\n";
            return 1;
        }
        auto r = mgr.smart_info(argv[2]);
        if (!r.has_value()) {
            std::cerr << "Error: " << r.error().message() << "\n";
            return 1;
        }
        const auto& info = r.value();

        std::cout << "SMART Health: " << argv[2] << "\n\n";
        std::cout << "  Overall:     " << info.overall_assessment << "\n";
        if (!info.model.empty())    std::cout << "  Model:       " << info.model << "\n";
        if (!info.serial.empty())   std::cout << "  Serial:      " << info.serial << "\n";
        if (!info.firmware.empty()) std::cout << "  Firmware:    " << info.firmware << "\n";
        if (info.power_on_hours > 0)
            std::cout << "  Power-on:    " << info.power_on_hours << " hours\n";
        if (info.power_cycle_count > 0)
            std::cout << "  Cycles:      " << info.power_cycle_count << "\n";
        if (info.temperature_celsius > 0)
            std::cout << "  Temperature: " << std::fixed << std::setprecision(0)
                      << info.temperature_celsius << " C\n";

        if (!info.attributes.empty()) {
            std::cout << "\n  Attributes:\n";
            std::cout << "  " << std::left
                      << std::setw(4) << "ID"
                      << std::setw(25) << "Name"
                      << std::setw(8) << "Value"
                      << std::setw(8) << "Worst"
                      << std::setw(8) << "Thresh"
                      << std::setw(12) << "Raw"
                      << "Status\n";
            std::cout << "  " << std::string(70, '-') << "\n";

            for (const auto& attr : info.attributes) {
                std::cout << "  " << std::setw(4) << static_cast<int>(attr.id)
                          << std::setw(25) << attr.name
                          << std::setw(8) << static_cast<int>(attr.current)
                          << std::setw(8) << static_cast<int>(attr.worst)
                          << std::setw(8) << static_cast<int>(attr.threshold)
                          << std::setw(12) << attr.raw_value
                          << attr.status << "\n";
            }
        }

    } else if (cmd == "encrypt") {
        if (argc < 3) {
            std::cerr << "Usage: straylight-disk encrypt <device>\n";
            return 1;
        }
        auto r = mgr.encrypt(argv[2]);
        if (!r.has_value()) {
            std::cerr << "Error: " << r.error().message() << "\n";
            return 1;
        }
        std::cout << "Device " << argv[2] << " encrypted with LUKS2.\n";

    } else if (cmd == "benchmark") {
        if (argc < 3) {
            std::cerr << "Usage: straylight-disk benchmark <device>\n";
            return 1;
        }
        std::cerr << "Running benchmarks on " << argv[2] << " ...\n";
        auto r = mgr.benchmark(argv[2]);
        if (!r.has_value()) {
            std::cerr << "Error: " << r.error().message() << "\n";
            return 1;
        }
        const auto& b = r.value();
        std::cout << "\nBenchmark Results: " << b.device << "\n\n";
        std::cout << std::fixed << std::setprecision(1);
        std::cout << "  Sequential Read:  " << b.seq_read_mbps << " MB/s\n";
        std::cout << "  Sequential Write: " << b.seq_write_mbps << " MB/s\n";
        if (b.rand_read_iops > 0)
            std::cout << "  Random Read:      " << b.rand_read_iops << " IOPS\n";
        if (b.rand_write_iops > 0)
            std::cout << "  Random Write:     " << b.rand_write_iops << " IOPS\n";
        if (b.latency_us > 0)
            std::cout << "  Avg Latency:      " << b.latency_us << " us\n";

    } else if (cmd == "eject") {
        if (argc < 3) {
            std::cerr << "Usage: straylight-disk eject <device>\n";
            return 1;
        }
        auto r = mgr.eject(argv[2]);
        if (!r.has_value()) {
            std::cerr << "Error: " << r.error().message() << "\n";
            return 1;
        }
        std::cout << "Device " << argv[2] << " safely ejected.\n";

    } else {
        std::cerr << "Error: unknown command '" << cmd << "'\n";
        print_usage();
        return 1;
    }

    return 0;
}
