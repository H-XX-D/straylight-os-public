// tools/boot/boot_manager.h
#pragma once

#include <straylight/result.h>
#include <straylight/error.h>

#include <cstdint>
#include <string>
#include <vector>

namespace straylight {

/// Bootloader type detected on the system.
enum class BootloaderType : uint8_t {
    Grub2       = 0,
    SystemdBoot = 1,
    Unknown     = 2,
};

/// Information about an installed kernel.
struct KernelInfo {
    std::string version;        // e.g., "6.1.0-23-amd64"
    std::string path;           // /boot/vmlinuz-6.1.0-23-amd64
    std::string initrd_path;    // /boot/initrd.img-6.1.0-23-amd64
    std::string config_path;    // /boot/config-6.1.0-23-amd64
    bool is_default = false;
    bool is_running = false;
};

/// Current boot status.
struct BootStatus {
    std::string current_kernel;
    std::string cmdline;
    BootloaderType bootloader = BootloaderType::Unknown;
    std::string bootloader_version;
    std::string root_device;
    std::string boot_mode;      // "UEFI" or "BIOS/Legacy"
    bool secure_boot = false;
    int default_timeout = 5;
};

/// GRUB configuration parameters.
struct GrubConfig {
    std::string cmdline_default;
    std::string cmdline_linux;
    int timeout = 5;
    std::string default_entry;
    std::string distributor;
};

/// systemd-boot entry.
struct SystemdBootEntry {
    std::string id;             // Entry filename without .conf
    std::string title;
    std::string linux_path;
    std::string initrd_path;
    std::string options;
    bool is_default = false;
};

/// Boot manager: bootloader configuration, kernel management, boot parameters.
class BootManager {
public:
    /// Detect which bootloader is in use.
    BootloaderType detect_bootloader() const;

    /// List all installed kernels.
    Result<std::vector<KernelInfo>, SLError> list_kernels() const;

    /// Set the default kernel/entry.
    Result<void, SLError> set_default(const std::string& kernel_version) const;

    /// Add a kernel boot parameter.
    Result<void, SLError> add_param(const std::string& param) const;

    /// Remove a kernel boot parameter.
    Result<void, SLError> remove_param(const std::string& param) const;

    /// Set the bootloader timeout.
    Result<void, SLError> set_timeout(int seconds) const;

    /// Show current boot configuration.
    Result<BootStatus, SLError> get_status() const;

    /// Show full bootloader configuration.
    Result<std::string, SLError> show_config() const;

    /// Rebuild the initramfs for the current or specified kernel.
    Result<void, SLError> rebuild_initramfs(const std::string& version = "") const;

    // -----------------------------------------------------------------------
    // GRUB-specific
    // -----------------------------------------------------------------------

    /// Parse /etc/default/grub.
    Result<GrubConfig, SLError> parse_grub_config() const;

    /// Write /etc/default/grub and run update-grub.
    Result<void, SLError> write_grub_config(const GrubConfig& config) const;

    // -----------------------------------------------------------------------
    // systemd-boot specific
    // -----------------------------------------------------------------------

    /// List systemd-boot entries.
    Result<std::vector<SystemdBootEntry>, SLError> list_sd_boot_entries() const;

    /// Parse a systemd-boot entry file.
    Result<SystemdBootEntry, SLError> parse_sd_boot_entry(const std::string& path) const;

private:
    /// Execute a command and capture output.
    Result<std::string, SLError> exec_cmd(const std::string& cmd) const;

    /// Get the currently running kernel version.
    std::string running_kernel() const;

    /// Read /proc/cmdline.
    std::string current_cmdline() const;

    /// Modify GRUB_CMDLINE_LINUX_DEFAULT in /etc/default/grub.
    Result<void, SLError> modify_grub_cmdline(const std::string& param, bool add) const;

    /// Modify the options line in a systemd-boot entry.
    Result<void, SLError> modify_sd_boot_options(const std::string& param, bool add) const;

    /// Run update-grub or bootctl update.
    Result<void, SLError> update_bootloader() const;
};

} // namespace straylight
