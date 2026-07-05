// tools/disk/disk_manager.cpp
#include "disk_manager.h"

#include <straylight/log.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <sys/statvfs.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace straylight {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

Result<std::string, SLError> DiskManager::exec_cmd(const std::string& cmd) const {
    std::array<char, 4096> buf;
    std::string output;

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return Result<std::string, SLError>::error(
            {SLErrorCode::IOError, "Failed to execute: " + cmd});
    }

    while (fgets(buf.data(), buf.size(), pipe) != nullptr) {
        output += buf.data();
    }

    int rc = pclose(pipe);
    if (rc != 0) {
        return Result<std::string, SLError>::error(
            {SLErrorCode::IOError, "Command failed (exit " + std::to_string(rc) + "): " + cmd});
    }

    return Result<std::string, SLError>::ok(std::move(output));
}

uint64_t DiskManager::parse_size(const std::string& size_str) {
    if (size_str.empty()) return 0;

    char suffix = size_str.back();
    double value = 0;
    try {
        value = std::stod(size_str.substr(0, size_str.size() - (std::isalpha(suffix) ? 1 : 0)));
    } catch (...) {
        return 0;
    }

    switch (std::toupper(suffix)) {
        case 'T': return static_cast<uint64_t>(value * 1024ULL * 1024 * 1024 * 1024);
        case 'G': return static_cast<uint64_t>(value * 1024ULL * 1024 * 1024);
        case 'M': return static_cast<uint64_t>(value * 1024ULL * 1024);
        case 'K': return static_cast<uint64_t>(value * 1024ULL);
        default:  return static_cast<uint64_t>(value);
    }
}

std::string DiskManager::format_bytes(uint64_t bytes) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1);

    if (bytes >= 1024ULL * 1024 * 1024 * 1024) {
        oss << static_cast<double>(bytes) / (1024.0 * 1024 * 1024 * 1024) << " TiB";
    } else if (bytes >= 1024ULL * 1024 * 1024) {
        oss << static_cast<double>(bytes) / (1024.0 * 1024 * 1024) << " GiB";
    } else if (bytes >= 1024ULL * 1024) {
        oss << static_cast<double>(bytes) / (1024.0 * 1024) << " MiB";
    } else if (bytes >= 1024) {
        oss << static_cast<double>(bytes) / 1024.0 << " KiB";
    } else {
        oss << bytes << " B";
    }
    return oss.str();
}

// ---------------------------------------------------------------------------
// List devices
// ---------------------------------------------------------------------------

Result<std::vector<BlockDevice>, SLError> DiskManager::parse_lsblk() const {
    auto result = exec_cmd(
        "lsblk --json --bytes --output "
        "NAME,PATH,MODEL,SERIAL,TYPE,FSTYPE,LABEL,MOUNTPOINT,UUID,SIZE,RM,RO,TRAN "
        "2>/dev/null");

    if (!result.has_value()) {
        return Result<std::vector<BlockDevice>, SLError>::error(result.error());
    }

    const std::string& json = result.value();
    std::vector<BlockDevice> devices;

    // Simple JSON field extraction since we can't depend on nlohmann/json
    // at the tool level without linking against it (though straylight-common has it).
    // We'll parse the JSON manually.

    auto extract_string = [](const std::string& src, const std::string& key,
                             size_t start) -> std::pair<std::string, size_t> {
        auto kpos = src.find("\"" + key + "\"", start);
        if (kpos == std::string::npos) return {"", std::string::npos};
        auto colon = src.find(':', kpos);
        // Check for null.
        auto val_start = src.find_first_not_of(" \t\n", colon + 1);
        if (val_start == std::string::npos) return {"", std::string::npos};
        if (src[val_start] == 'n') return {"", val_start + 4}; // null

        if (src[val_start] == '"') {
            auto q_end = src.find('"', val_start + 1);
            if (q_end == std::string::npos) return {"", std::string::npos};
            return {src.substr(val_start + 1, q_end - val_start - 1), q_end + 1};
        }
        // Numeric or boolean.
        auto end = src.find_first_of(",}\n]", val_start);
        std::string val = src.substr(val_start, end - val_start);
        // Trim whitespace.
        while (!val.empty() && (val.back() == ' ' || val.back() == '\n')) val.pop_back();
        return {val, end};
    };

    // Find each device object.
    size_t pos = 0;
    while (true) {
        auto obj_start = json.find('{', pos);
        if (obj_start == std::string::npos) break;
        auto obj_end = json.find('}', obj_start);
        if (obj_end == std::string::npos) break;

        // Check for nested children array — skip inner objects handled separately.
        // Just parse the flat object.
        std::string obj = json.substr(obj_start, obj_end - obj_start + 1);

        BlockDevice dev;
        dev.name = extract_string(obj, "name", 0).first;
        dev.path = extract_string(obj, "path", 0).first;
        dev.model = extract_string(obj, "model", 0).first;
        dev.serial = extract_string(obj, "serial", 0).first;
        dev.type = extract_string(obj, "type", 0).first;
        dev.fstype = extract_string(obj, "fstype", 0).first;
        dev.label = extract_string(obj, "label", 0).first;
        dev.mountpoint = extract_string(obj, "mountpoint", 0).first;
        dev.uuid = extract_string(obj, "uuid", 0).first;
        dev.transport = extract_string(obj, "tran", 0).first;

        auto size_str = extract_string(obj, "size", 0).first;
        if (!size_str.empty()) {
            try { dev.size_bytes = std::stoull(size_str); } catch (...) {}
        }

        dev.removable = extract_string(obj, "rm", 0).first == "true" ||
                        extract_string(obj, "rm", 0).first == "1";
        dev.readonly = extract_string(obj, "ro", 0).first == "true" ||
                       extract_string(obj, "ro", 0).first == "1";

        if (!dev.name.empty()) {
            devices.push_back(std::move(dev));
        }

        pos = obj_end + 1;
    }

    return Result<std::vector<BlockDevice>, SLError>::ok(std::move(devices));
}

Result<std::vector<BlockDevice>, SLError> DiskManager::scan_sysfs() const {
    std::vector<BlockDevice> devices;
    std::string sys_block = "/sys/block";

    if (!fs::exists(sys_block)) {
        return Result<std::vector<BlockDevice>, SLError>::error(
            {SLErrorCode::NotFound, "/sys/block not found"});
    }

    std::error_code ec;
    for (auto& entry : fs::directory_iterator(sys_block, ec)) {
        if (ec) continue;

        std::string name = entry.path().filename().string();
        // Skip loop and ram devices.
        if (name.substr(0, 4) == "loop" || name.substr(0, 3) == "ram") continue;

        BlockDevice dev;
        dev.name = name;
        dev.path = "/dev/" + name;
        dev.type = "disk";

        // Read size (in 512-byte sectors).
        auto size_path = entry.path() / "size";
        if (fs::exists(size_path)) {
            std::ifstream ifs(size_path);
            uint64_t sectors = 0;
            if (ifs >> sectors) {
                dev.size_bytes = sectors * 512;
            }
        }

        // Read model.
        auto model_path = entry.path() / "device" / "model";
        if (fs::exists(model_path)) {
            std::ifstream ifs(model_path);
            std::getline(ifs, dev.model);
            // Trim trailing whitespace.
            while (!dev.model.empty() && std::isspace(dev.model.back())) {
                dev.model.pop_back();
            }
        }

        // Read removable flag.
        auto rm_path = entry.path() / "removable";
        if (fs::exists(rm_path)) {
            std::ifstream ifs(rm_path);
            int rm = 0;
            if (ifs >> rm) dev.removable = (rm != 0);
        }

        // Read ro flag.
        auto ro_path = entry.path() / "ro";
        if (fs::exists(ro_path)) {
            std::ifstream ifs(ro_path);
            int ro = 0;
            if (ifs >> ro) dev.readonly = (ro != 0);
        }

        // Detect transport.
        auto symlink_path = entry.path() / "device";
        if (fs::exists(symlink_path)) {
            std::string resolved = fs::canonical(symlink_path, ec).string();
            if (resolved.find("usb") != std::string::npos) dev.transport = "usb";
            else if (resolved.find("nvme") != std::string::npos) dev.transport = "nvme";
            else if (resolved.find("ata") != std::string::npos) dev.transport = "sata";
            else dev.transport = "scsi";
        }

        // Scan partitions.
        for (auto& sub : fs::directory_iterator(entry.path(), ec)) {
            if (ec) continue;
            std::string subname = sub.path().filename().string();
            if (subname.find(name) != 0) continue;
            if (!fs::exists(sub.path() / "partition")) continue;

            BlockDevice part;
            part.name = subname;
            part.path = "/dev/" + subname;
            part.type = "part";

            auto part_size_path = sub.path() / "size";
            if (fs::exists(part_size_path)) {
                std::ifstream ifs(part_size_path);
                uint64_t sectors = 0;
                if (ifs >> sectors) part.size_bytes = sectors * 512;
            }

            dev.children.push_back(std::move(part));
        }

        devices.push_back(std::move(dev));
    }

    return Result<std::vector<BlockDevice>, SLError>::ok(std::move(devices));
}

Result<std::vector<BlockDevice>, SLError> DiskManager::list_devices() const {
    // Try lsblk first (richer data), fall back to sysfs.
    auto lsblk_result = parse_lsblk();
    if (lsblk_result.has_value() && !lsblk_result.value().empty()) {
        return lsblk_result;
    }
    return scan_sysfs();
}

Result<BlockDevice, SLError> DiskManager::device_info(const std::string& device) const {
    auto devices = list_devices();
    if (!devices.has_value()) return Result<BlockDevice, SLError>::error(devices.error());

    std::string search = device;
    // Normalize: if no /dev/ prefix, add it.
    if (search.find("/dev/") != 0) search = "/dev/" + search;

    for (const auto& dev : devices.value()) {
        if (dev.path == search || dev.name == device) {
            return Result<BlockDevice, SLError>::ok(dev);
        }
        for (const auto& child : dev.children) {
            if (child.path == search || child.name == device) {
                return Result<BlockDevice, SLError>::ok(child);
            }
        }
    }

    return Result<BlockDevice, SLError>::error(
        {SLErrorCode::NotFound, "Device not found: " + device});
}

// ---------------------------------------------------------------------------
// Mount / Unmount
// ---------------------------------------------------------------------------

Result<void, SLError> DiskManager::mount(const std::string& device,
                                          const std::string& path,
                                          const std::string& options) const {
    // Ensure mountpoint exists.
    std::error_code ec;
    fs::create_directories(path, ec);
    if (ec) {
        return Result<void, SLError>::error(
            {SLErrorCode::IOError, "Cannot create mountpoint: " + ec.message()});
    }

    std::string cmd = "mount";
    if (!options.empty()) {
        cmd += " -o " + options;
    }
    cmd += " " + device + " " + path + " 2>&1";

    auto result = exec_cmd(cmd);
    if (!result.has_value()) {
        return Result<void, SLError>::error(
            {SLErrorCode::PermissionDenied,
             "Mount failed: " + result.error().message()});
    }

    SL_INFO("disk: mounted {} at {}", device, path);
    return Result<void, SLError>::ok();
}

Result<void, SLError> DiskManager::unmount(const std::string& path) const {
    std::string cmd = "umount " + path + " 2>&1";
    auto result = exec_cmd(cmd);
    if (!result.has_value()) {
        // Try lazy unmount.
        cmd = "umount -l " + path + " 2>&1";
        result = exec_cmd(cmd);
        if (!result.has_value()) {
            return Result<void, SLError>::error(
                {SLErrorCode::IOError, "Unmount failed: " + result.error().message()});
        }
    }
    SL_INFO("disk: unmounted {}", path);
    return Result<void, SLError>::ok();
}

// ---------------------------------------------------------------------------
// Format
// ---------------------------------------------------------------------------

Result<void, SLError> DiskManager::format(const std::string& device,
                                           const std::string& fstype,
                                           const std::string& label) const {
    // Validate device path.
    if (device.find("/dev/") != 0) {
        return Result<void, SLError>::error(
            {SLErrorCode::InvalidArgument, "Device must start with /dev/"});
    }

    // Build mkfs command.
    std::string cmd;
    if (fstype == "ext4") {
        cmd = "mkfs.ext4 -F";
        if (!label.empty()) cmd += " -L '" + label + "'";
    } else if (fstype == "btrfs") {
        cmd = "mkfs.btrfs -f";
        if (!label.empty()) cmd += " -L '" + label + "'";
    } else if (fstype == "xfs") {
        cmd = "mkfs.xfs -f";
        if (!label.empty()) cmd += " -L '" + label + "'";
    } else if (fstype == "ntfs") {
        cmd = "mkfs.ntfs -Q";
        if (!label.empty()) cmd += " -L '" + label + "'";
    } else if (fstype == "fat32" || fstype == "vfat") {
        cmd = "mkfs.vfat -F 32";
        if (!label.empty()) cmd += " -n '" + label + "'";
    } else if (fstype == "exfat") {
        cmd = "mkfs.exfat";
        if (!label.empty()) cmd += " -n '" + label + "'";
    } else if (fstype == "swap") {
        cmd = "mkswap";
        if (!label.empty()) cmd += " -L '" + label + "'";
    } else {
        return Result<void, SLError>::error(
            {SLErrorCode::InvalidArgument,
             "Unsupported filesystem: " + fstype +
             " (supported: ext4, btrfs, xfs, ntfs, fat32, exfat, swap)"});
    }

    cmd += " " + device + " 2>&1";

    auto result = exec_cmd(cmd);
    if (!result.has_value()) {
        return Result<void, SLError>::error(
            {SLErrorCode::IOError, "Format failed: " + result.error().message()});
    }

    SL_INFO("disk: formatted {} as {}", device, fstype);
    return Result<void, SLError>::ok();
}

// ---------------------------------------------------------------------------
// Resize
// ---------------------------------------------------------------------------

Result<void, SLError> DiskManager::resize(const std::string& device,
                                           const std::string& size) const {
    // Determine filesystem type.
    auto info = device_info(device);
    if (!info.has_value()) return Result<void, SLError>::error(info.error());

    const auto& dev = info.value();
    std::string cmd;

    if (dev.fstype == "ext4" || dev.fstype == "ext3" || dev.fstype == "ext2") {
        cmd = "resize2fs " + device;
        if (!size.empty()) cmd += " " + size;
    } else if (dev.fstype == "btrfs") {
        if (dev.mountpoint.empty()) {
            return Result<void, SLError>::error(
                {SLErrorCode::InvalidArgument, "btrfs must be mounted to resize"});
        }
        cmd = "btrfs filesystem resize " + (size.empty() ? "max" : size) + " " + dev.mountpoint;
    } else if (dev.fstype == "xfs") {
        if (dev.mountpoint.empty()) {
            return Result<void, SLError>::error(
                {SLErrorCode::InvalidArgument, "XFS must be mounted to grow"});
        }
        cmd = "xfs_growfs " + dev.mountpoint;
    } else {
        return Result<void, SLError>::error(
            {SLErrorCode::InvalidArgument,
             "Resize not supported for filesystem: " + dev.fstype});
    }

    cmd += " 2>&1";
    auto result = exec_cmd(cmd);
    if (!result.has_value()) {
        return Result<void, SLError>::error(
            {SLErrorCode::IOError, "Resize failed: " + result.error().message()});
    }

    SL_INFO("disk: resized {} to {}", device, size.empty() ? "max" : size);
    return Result<void, SLError>::ok();
}

// ---------------------------------------------------------------------------
// SMART
// ---------------------------------------------------------------------------

Result<SmartInfo, SLError> DiskManager::smart_info(const std::string& device) const {
    auto result = exec_cmd("smartctl -a -j " + device + " 2>/dev/null");
    if (!result.has_value()) {
        // Try without JSON.
        result = exec_cmd("smartctl -a " + device + " 2>/dev/null");
        if (!result.has_value()) {
            return Result<SmartInfo, SLError>::error(
                {SLErrorCode::IOError, "smartctl failed — is smartmontools installed?"});
        }
    }

    const std::string& output = result.value();
    SmartInfo info;
    info.device = device;

    // Parse smartctl text output.
    std::istringstream iss(output);
    std::string line;
    bool in_attributes = false;

    while (std::getline(iss, line)) {
        if (line.find("Device Model:") != std::string::npos ||
            line.find("Model Number:") != std::string::npos) {
            auto pos = line.find(':');
            info.model = line.substr(pos + 1);
            while (!info.model.empty() && info.model[0] == ' ') info.model.erase(0, 1);
        }
        else if (line.find("Serial Number:") != std::string::npos) {
            auto pos = line.find(':');
            info.serial = line.substr(pos + 1);
            while (!info.serial.empty() && info.serial[0] == ' ') info.serial.erase(0, 1);
        }
        else if (line.find("Firmware Version:") != std::string::npos) {
            auto pos = line.find(':');
            info.firmware = line.substr(pos + 1);
            while (!info.firmware.empty() && info.firmware[0] == ' ') info.firmware.erase(0, 1);
        }
        else if (line.find("SMART overall-health") != std::string::npos) {
            info.overall_assessment = (line.find("PASSED") != std::string::npos) ? "PASSED" : "FAILED";
            info.healthy = (line.find("PASSED") != std::string::npos);
        }
        else if (line.find("Power_On_Hours") != std::string::npos ||
                 line.find("Power On Hours") != std::string::npos) {
            // Try to extract the raw value (last number on the line).
            auto pos = line.rfind(' ');
            if (pos != std::string::npos) {
                try { info.power_on_hours = std::stoull(line.substr(pos + 1)); } catch (...) {}
            }
        }
        else if (line.find("Power_Cycle_Count") != std::string::npos) {
            auto pos = line.rfind(' ');
            if (pos != std::string::npos) {
                try { info.power_cycle_count = std::stoull(line.substr(pos + 1)); } catch (...) {}
            }
        }
        else if (line.find("Temperature_Celsius") != std::string::npos ||
                 line.find("Temperature:") != std::string::npos) {
            auto pos = line.rfind(' ');
            if (pos != std::string::npos) {
                try { info.temperature_celsius = std::stod(line.substr(pos + 1)); } catch (...) {}
            }
        }
        else if (line.find("ID#") != std::string::npos && line.find("ATTRIBUTE_NAME") != std::string::npos) {
            in_attributes = true;
            continue;
        }

        if (in_attributes && !line.empty() && std::isdigit(static_cast<unsigned char>(line[0]))) {
            // Parse attribute line: ID# ATTRIBUTE_NAME FLAG VALUE WORST THRESH TYPE UPDATED WHEN_FAILED RAW_VALUE
            std::istringstream attr_iss(line);
            SmartAttribute attr;
            std::string name, flag, type, updated, when_failed;
            int id_int = 0, current_int = 0, worst_int = 0, thresh_int = 0;
            uint64_t raw = 0;

            if (attr_iss >> id_int >> name >> flag >> current_int >> worst_int >> thresh_int
                        >> type >> updated >> when_failed >> raw) {
                attr.id = static_cast<uint8_t>(id_int);
                attr.name = name;
                attr.current = static_cast<uint8_t>(current_int);
                attr.worst = static_cast<uint8_t>(worst_int);
                attr.threshold = static_cast<uint8_t>(thresh_int);
                attr.raw_value = raw;

                if (attr.current <= attr.threshold && attr.threshold > 0) {
                    attr.status = "FAILING";
                } else if (attr.worst <= attr.threshold && attr.threshold > 0) {
                    attr.status = "WARNING";
                } else {
                    attr.status = "OK";
                }

                info.attributes.push_back(attr);
            }
        }
    }

    if (info.overall_assessment.empty()) {
        info.overall_assessment = "UNKNOWN";
    }

    return Result<SmartInfo, SLError>::ok(std::move(info));
}

// ---------------------------------------------------------------------------
// Encrypt
// ---------------------------------------------------------------------------

Result<void, SLError> DiskManager::encrypt(const std::string& device) const {
    // Verify device exists.
    if (!fs::exists(device)) {
        return Result<void, SLError>::error(
            {SLErrorCode::NotFound, "Device not found: " + device});
    }

    // Check if cryptsetup is available.
    int rc = std::system("which cryptsetup >/dev/null 2>&1");
    if (rc != 0) {
        return Result<void, SLError>::error(
            {SLErrorCode::NotFound, "cryptsetup not found — install it with: apt install cryptsetup"});
    }

    // Format with LUKS2.
    std::string cmd = "cryptsetup luksFormat --type luks2 --cipher aes-xts-plain64 "
                      "--key-size 512 --hash sha512 --iter-time 5000 " +
                      device + " 2>&1";

    std::cerr << "WARNING: This will destroy all data on " << device << "!\n";
    std::cerr << "Running: cryptsetup luksFormat " << device << "\n";

    auto result = exec_cmd(cmd);
    if (!result.has_value()) {
        return Result<void, SLError>::error(
            {SLErrorCode::IOError, "LUKS format failed: " + result.error().message()});
    }

    SL_INFO("disk: encrypted {} with LUKS2", device);
    return Result<void, SLError>::ok();
}

// ---------------------------------------------------------------------------
// Benchmark
// ---------------------------------------------------------------------------

Result<BenchmarkResult, SLError> DiskManager::benchmark(const std::string& device) const {
    BenchmarkResult bench;
    bench.device = device;

    // Sequential read benchmark using dd.
    auto read_result = exec_cmd(
        "dd if=" + device + " of=/dev/null bs=1M count=256 iflag=direct 2>&1 | tail -1");
    if (read_result.has_value()) {
        const auto& line = read_result.value();
        // Parse: "268435456 bytes (268 MB, 256 MiB) copied, 0.531s, 505 MB/s"
        auto mbps_pos = line.rfind("MB/s");
        if (mbps_pos == std::string::npos) mbps_pos = line.rfind("GB/s");
        if (mbps_pos != std::string::npos) {
            auto comma = line.rfind(',', mbps_pos);
            if (comma != std::string::npos) {
                std::string speed_str = line.substr(comma + 1, mbps_pos - comma - 1);
                try {
                    bench.seq_read_mbps = std::stod(speed_str);
                    if (line.find("GB/s") != std::string::npos) bench.seq_read_mbps *= 1024;
                } catch (...) {}
            }
        }
    }

    // Sequential write benchmark (only on non-system devices).
    // Use a temporary file if we can detect a mountpoint.
    auto dev_info = device_info(device);
    std::string write_target = "/tmp/straylight-disk-bench.tmp";

    auto write_result = exec_cmd(
        "dd if=/dev/zero of=" + write_target + " bs=1M count=256 oflag=direct conv=fdatasync 2>&1 | tail -1");
    if (write_result.has_value()) {
        const auto& line = write_result.value();
        auto mbps_pos = line.rfind("MB/s");
        if (mbps_pos == std::string::npos) mbps_pos = line.rfind("GB/s");
        if (mbps_pos != std::string::npos) {
            auto comma = line.rfind(',', mbps_pos);
            if (comma != std::string::npos) {
                std::string speed_str = line.substr(comma + 1, mbps_pos - comma - 1);
                try {
                    bench.seq_write_mbps = std::stod(speed_str);
                    if (line.find("GB/s") != std::string::npos) bench.seq_write_mbps *= 1024;
                } catch (...) {}
            }
        }
    }

    // Clean up temp file.
    std::error_code ec;
    fs::remove(write_target, ec);

    // Random I/O benchmark using fio if available, otherwise estimate.
    auto fio_result = exec_cmd(
        "fio --name=randread --filename=" + device +
        " --ioengine=libaio --direct=1 --rw=randread --bs=4k "
        "--numjobs=1 --runtime=5 --time_based --size=128M "
        "--output-format=terse 2>/dev/null | head -1");
    if (fio_result.has_value() && !fio_result.value().empty()) {
        // Terse format field 8 is read IOPS.
        std::istringstream fio_iss(fio_result.value());
        std::string field;
        int field_num = 0;
        while (std::getline(fio_iss, field, ';')) {
            if (field_num == 7) {
                try { bench.rand_read_iops = std::stod(field); } catch (...) {}
                break;
            }
            ++field_num;
        }
    }

    // Random write IOPS.
    auto fio_write_result = exec_cmd(
        "fio --name=randwrite --filename=" + write_target +
        " --ioengine=libaio --direct=1 --rw=randwrite --bs=4k "
        "--numjobs=1 --runtime=5 --time_based --size=128M "
        "--output-format=terse 2>/dev/null | head -1");
    if (fio_write_result.has_value() && !fio_write_result.value().empty()) {
        std::istringstream fio_iss(fio_write_result.value());
        std::string field;
        int field_num = 0;
        while (std::getline(fio_iss, field, ';')) {
            if (field_num == 48) { // Write IOPS field.
                try { bench.rand_write_iops = std::stod(field); } catch (...) {}
                break;
            }
            ++field_num;
        }
    }

    // Clean up.
    fs::remove(write_target, ec);

    // Simple latency test using ioping if available.
    auto lat_result = exec_cmd("ioping -c 10 -q " + device + " 2>/dev/null | tail -1");
    if (lat_result.has_value()) {
        const auto& line = lat_result.value();
        // Parse "min/avg/max/mdev = 30/35/50/5 us"
        auto eq_pos = line.find('=');
        if (eq_pos != std::string::npos) {
            // Extract avg (second field after =, separated by /).
            std::string values = line.substr(eq_pos + 2);
            auto slash1 = values.find('/');
            if (slash1 != std::string::npos) {
                auto slash2 = values.find('/', slash1 + 1);
                if (slash2 != std::string::npos) {
                    try {
                        bench.latency_us = std::stod(values.substr(slash1 + 1, slash2 - slash1 - 1));
                        // Check unit.
                        if (values.find("ms") != std::string::npos) bench.latency_us *= 1000;
                    } catch (...) {}
                }
            }
        }
    }

    return Result<BenchmarkResult, SLError>::ok(std::move(bench));
}

// ---------------------------------------------------------------------------
// Filesystem usage
// ---------------------------------------------------------------------------

Result<FsUsage, SLError> DiskManager::fs_usage(const std::string& mountpoint) const {
    struct statvfs st;
    if (statvfs(mountpoint.c_str(), &st) != 0) {
        return Result<FsUsage, SLError>::error(
            {SLErrorCode::IOError, "statvfs failed for: " + mountpoint});
    }

    FsUsage usage;
    usage.mountpoint = mountpoint;
    usage.total_bytes = static_cast<uint64_t>(st.f_blocks) * st.f_frsize;
    usage.available_bytes = static_cast<uint64_t>(st.f_bavail) * st.f_frsize;
    usage.used_bytes = usage.total_bytes -
                       static_cast<uint64_t>(st.f_bfree) * st.f_frsize;
    usage.total_inodes = st.f_files;
    usage.used_inodes = st.f_files - st.f_ffree;
    usage.usage_percent = (usage.total_bytes > 0)
        ? (static_cast<double>(usage.used_bytes) / usage.total_bytes * 100.0)
        : 0.0;

    return Result<FsUsage, SLError>::ok(std::move(usage));
}

// ---------------------------------------------------------------------------
// Eject
// ---------------------------------------------------------------------------

Result<void, SLError> DiskManager::eject(const std::string& device) const {
    // First unmount all partitions.
    auto devices = list_devices();
    if (devices.has_value()) {
        for (const auto& dev : devices.value()) {
            if (dev.path == device || dev.name == device) {
                if (!dev.mountpoint.empty()) {
                    unmount(dev.mountpoint);
                }
                for (const auto& child : dev.children) {
                    if (!child.mountpoint.empty()) {
                        unmount(child.mountpoint);
                    }
                }
                break;
            }
        }
    }

    // Power off the USB device.
    std::string cmd = "udisksctl power-off -b " + device + " 2>&1";
    auto result = exec_cmd(cmd);
    if (!result.has_value()) {
        // Fallback to eject command.
        cmd = "eject " + device + " 2>&1";
        result = exec_cmd(cmd);
        if (!result.has_value()) {
            return Result<void, SLError>::error(
                {SLErrorCode::IOError, "Eject failed: " + result.error().message()});
        }
    }

    SL_INFO("disk: ejected {}", device);
    return Result<void, SLError>::ok();
}

} // namespace straylight
