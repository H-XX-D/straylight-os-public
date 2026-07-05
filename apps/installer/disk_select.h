// apps/installer/disk_select.h
// Block device enumeration and installation target selection
#pragma once

#include <string>
#include <vector>

namespace straylight::installer {

/// Represents a single block device (whole disk, not partition) suitable
/// as an installation target.
struct DiskInfo {
    std::string name;         // "nvme0n1", "sda"
    std::string path;         // "/dev/nvme0n1"
    std::string model;        // "Samsung SSD 980 PRO 1TB"
    std::string size;         // "931.5G"
    std::string transport;    // "nvme", "sata", "usb"
    bool        removable  = false;
    bool        has_partitions = false;
    bool        has_known_os   = false;  // existing OS detected in partitions
};

/// Scan all block devices and return a list of candidate install targets.
/// Excludes loop devices, CD-ROMs, and the live boot medium.
std::vector<DiskInfo> enumerate_disks();

/// Parse a JSON blob from `lsblk -J` and return DiskInfo entries.
/// Exposed for unit testing.
std::vector<DiskInfo> parse_lsblk_json(const std::string& json);

/// Disk selection page — ImGui UI over enumerate_disks().
class DiskSelectPage {
public:
    DiskSelectPage();
    ~DiskSelectPage() = default;

    /// Render.  Returns true when the user has confirmed a selection.
    bool render();

    /// The disk the user confirmed, or nullptr if none yet.
    [[nodiscard]] const DiskInfo* selected() const;

private:
    std::vector<DiskInfo> disks_;
    int         selected_idx_   = -1;
    bool        scanned_        = false;
    bool        show_confirm_   = false;

    void do_scan();
};

} // namespace straylight::installer
