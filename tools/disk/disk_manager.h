// tools/disk/disk_manager.h
#pragma once

#include <straylight/result.h>
#include <straylight/error.h>

#include <cstdint>
#include <string>
#include <vector>

namespace straylight {

/// Information about a block device.
struct BlockDevice {
    std::string name;           // e.g., "sda", "nvme0n1"
    std::string path;           // e.g., "/dev/sda"
    std::string model;
    std::string serial;
    std::string type;           // "disk", "part", "loop", "rom"
    std::string fstype;         // "ext4", "btrfs", "ntfs", etc.
    std::string label;
    std::string mountpoint;
    std::string uuid;
    uint64_t size_bytes = 0;
    bool removable = false;
    bool readonly = false;
    std::string transport;      // "sata", "nvme", "usb", "scsi"
    std::vector<BlockDevice> children;  // Partitions
};

/// SMART attribute data.
struct SmartAttribute {
    uint8_t id;
    std::string name;
    uint64_t raw_value;
    uint8_t current;
    uint8_t worst;
    uint8_t threshold;
    std::string status;     // "OK", "WARNING", "FAILING"
};

/// SMART health data for a device.
struct SmartInfo {
    std::string device;
    std::string model;
    std::string serial;
    std::string firmware;
    bool healthy = true;
    std::string overall_assessment;
    uint64_t power_on_hours = 0;
    uint64_t power_cycle_count = 0;
    double temperature_celsius = 0.0;
    std::vector<SmartAttribute> attributes;
};

/// Filesystem usage information.
struct FsUsage {
    std::string mountpoint;
    std::string device;
    std::string fstype;
    uint64_t total_bytes = 0;
    uint64_t used_bytes = 0;
    uint64_t available_bytes = 0;
    double usage_percent = 0.0;
    uint64_t total_inodes = 0;
    uint64_t used_inodes = 0;
};

/// Disk benchmark results.
struct BenchmarkResult {
    std::string device;
    double seq_read_mbps = 0.0;
    double seq_write_mbps = 0.0;
    double rand_read_iops = 0.0;
    double rand_write_iops = 0.0;
    double latency_us = 0.0;
};

/// Disk manager: list, mount, format, SMART, encrypt, benchmark.
class DiskManager {
public:
    /// List all block devices.
    Result<std::vector<BlockDevice>, SLError> list_devices() const;

    /// Get detailed info for a specific device.
    Result<BlockDevice, SLError> device_info(const std::string& device) const;

    /// Mount a device at a given path.
    Result<void, SLError> mount(const std::string& device, const std::string& path,
                                const std::string& options = "") const;

    /// Unmount a path or device.
    Result<void, SLError> unmount(const std::string& path) const;

    /// Format a device with the given filesystem type.
    Result<void, SLError> format(const std::string& device, const std::string& fstype,
                                 const std::string& label = "") const;

    /// Resize a filesystem (grow or shrink).
    Result<void, SLError> resize(const std::string& device, const std::string& size) const;

    /// Get SMART health data for a device.
    Result<SmartInfo, SLError> smart_info(const std::string& device) const;

    /// Set up LUKS encryption on a device.
    Result<void, SLError> encrypt(const std::string& device) const;

    /// Run disk benchmarks on a device.
    Result<BenchmarkResult, SLError> benchmark(const std::string& device) const;

    /// Get filesystem usage for a mountpoint.
    Result<FsUsage, SLError> fs_usage(const std::string& mountpoint) const;

    /// Safely eject a USB device.
    Result<void, SLError> eject(const std::string& device) const;

private:
    /// Parse lsblk JSON output.
    Result<std::vector<BlockDevice>, SLError> parse_lsblk() const;

    /// Scan /sys/block/ for device info.
    Result<std::vector<BlockDevice>, SLError> scan_sysfs() const;

    /// Execute a command and capture output.
    Result<std::string, SLError> exec_cmd(const std::string& cmd) const;

    /// Parse a size string like "100G", "500M" into bytes.
    static uint64_t parse_size(const std::string& size_str);

    /// Format bytes into human-readable string.
    static std::string format_bytes(uint64_t bytes);
};

} // namespace straylight
