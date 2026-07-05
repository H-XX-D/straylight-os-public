// tools/boot/main.cpp
// straylight-boot — Boot manager for StrayLight OS.
#include "boot_manager.h"

#include <straylight/log.h>

#include <iostream>
#include <string>

using straylight::BootManager;
using straylight::BootloaderType;

static void print_usage() {
    std::cerr
        << "straylight-boot — Boot manager\n\n"
        << "Usage:\n"
        << "  straylight-boot list-kernels               List installed kernels\n"
        << "  straylight-boot set-default <kernel>        Set default boot kernel\n"
        << "  straylight-boot add-param <param>           Add kernel boot parameter\n"
        << "  straylight-boot remove-param <param>        Remove kernel boot parameter\n"
        << "  straylight-boot set-timeout <seconds>       Set bootloader timeout\n"
        << "  straylight-boot show-config                 Show bootloader configuration\n"
        << "  straylight-boot rebuild-initramfs [version]  Rebuild initramfs\n"
        << "  straylight-boot status                      Current boot info\n"
        << "\nExamples:\n"
        << "  straylight-boot list-kernels\n"
        << "  straylight-boot set-default 6.1.0-23-amd64\n"
        << "  straylight-boot add-param quiet\n"
        << "  straylight-boot add-param 'intel_iommu=on'\n"
        << "  straylight-boot remove-param splash\n"
        << "  straylight-boot set-timeout 3\n";
}

static const char* bl_name(BootloaderType bl) {
    switch (bl) {
        case BootloaderType::Grub2:       return "GRUB2";
        case BootloaderType::SystemdBoot: return "systemd-boot";
        default:                          return "Unknown";
    }
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

    BootManager mgr;

    if (cmd == "list-kernels") {
        auto r = mgr.list_kernels();
        if (!r.has_value()) {
            std::cerr << "Error: " << r.error().message() << "\n";
            return 1;
        }

        const auto& kernels = r.value();
        if (kernels.empty()) {
            std::cout << "No kernels found in /boot/.\n";
            return 0;
        }

        std::cout << "Installed kernels:\n\n";
        for (const auto& k : kernels) {
            std::cout << "  ";
            if (k.is_running) std::cout << "* ";
            else if (k.is_default) std::cout << "> ";
            else std::cout << "  ";

            std::cout << k.version;
            if (k.is_running) std::cout << " (running)";
            if (k.is_default) std::cout << " (default)";
            std::cout << "\n";

            std::cout << "    vmlinuz: " << k.path << "\n";
            if (!k.initrd_path.empty())
                std::cout << "    initrd:  " << k.initrd_path << "\n";
            if (!k.config_path.empty())
                std::cout << "    config:  " << k.config_path << "\n";
            std::cout << "\n";
        }

    } else if (cmd == "set-default") {
        if (argc < 3) {
            std::cerr << "Usage: straylight-boot set-default <kernel-version>\n";
            return 1;
        }
        auto r = mgr.set_default(argv[2]);
        if (!r.has_value()) {
            std::cerr << "Error: " << r.error().message() << "\n";
            return 1;
        }
        std::cout << "Default kernel set to " << argv[2] << "\n";

    } else if (cmd == "add-param") {
        if (argc < 3) {
            std::cerr << "Usage: straylight-boot add-param <parameter>\n";
            return 1;
        }
        auto r = mgr.add_param(argv[2]);
        if (!r.has_value()) {
            std::cerr << "Error: " << r.error().message() << "\n";
            return 1;
        }
        std::cout << "Added parameter: " << argv[2] << "\n";

    } else if (cmd == "remove-param") {
        if (argc < 3) {
            std::cerr << "Usage: straylight-boot remove-param <parameter>\n";
            return 1;
        }
        auto r = mgr.remove_param(argv[2]);
        if (!r.has_value()) {
            std::cerr << "Error: " << r.error().message() << "\n";
            return 1;
        }
        std::cout << "Removed parameter: " << argv[2] << "\n";

    } else if (cmd == "set-timeout") {
        if (argc < 3) {
            std::cerr << "Usage: straylight-boot set-timeout <seconds>\n";
            return 1;
        }
        int seconds = 0;
        try { seconds = std::stoi(argv[2]); } catch (...) {
            std::cerr << "Error: invalid timeout value\n";
            return 1;
        }
        auto r = mgr.set_timeout(seconds);
        if (!r.has_value()) {
            std::cerr << "Error: " << r.error().message() << "\n";
            return 1;
        }
        std::cout << "Bootloader timeout set to " << seconds << " seconds\n";

    } else if (cmd == "show-config") {
        auto r = mgr.show_config();
        if (!r.has_value()) {
            std::cerr << "Error: " << r.error().message() << "\n";
            return 1;
        }
        std::cout << r.value();

    } else if (cmd == "rebuild-initramfs") {
        std::string version = (argc >= 3) ? argv[2] : "";
        std::cerr << "Rebuilding initramfs"
                  << (version.empty() ? " for current kernel" : " for " + version)
                  << " ...\n";
        auto r = mgr.rebuild_initramfs(version);
        if (!r.has_value()) {
            std::cerr << "Error: " << r.error().message() << "\n";
            return 1;
        }
        std::cout << "Initramfs rebuilt successfully.\n";

    } else if (cmd == "status") {
        auto r = mgr.get_status();
        if (!r.has_value()) {
            std::cerr << "Error: " << r.error().message() << "\n";
            return 1;
        }
        const auto& s = r.value();

        std::cout << "Boot Status\n\n";
        std::cout << "  Kernel:      " << s.current_kernel << "\n";
        std::cout << "  Bootloader:  " << bl_name(s.bootloader);
        if (!s.bootloader_version.empty())
            std::cout << " (" << s.bootloader_version << ")";
        std::cout << "\n";
        std::cout << "  Boot Mode:   " << s.boot_mode << "\n";
        std::cout << "  Secure Boot: " << (s.secure_boot ? "enabled" : "disabled") << "\n";
        if (!s.root_device.empty())
            std::cout << "  Root Device: " << s.root_device << "\n";
        std::cout << "  Timeout:     " << s.default_timeout << "s\n";
        std::cout << "\n  Command Line:\n    " << s.cmdline << "\n";

    } else {
        std::cerr << "Error: unknown command '" << cmd << "'\n";
        print_usage();
        return 1;
    }

    return 0;
}
