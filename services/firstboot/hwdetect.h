// services/firstboot/hwdetect.h
// Hardware detection types and public API for straylight-hwdetect.
// Scans PCI/USB/ACPI buses, resolves kernel modules via modalias,
// probes firmware presence, and reports driver status.
#pragma once

#include <string>
#include <vector>

namespace straylight::hwdetect {

/// Driver readiness for a single device.
enum class DriverStatus {
    kLoaded,        ///< Module loaded and device bound to its driver.
    kBuiltin,       ///< Driver compiled into the kernel (no module file needed).
    kDkmsBuilding,  ///< DKMS module present but being compiled.
    kDkmsReady,     ///< DKMS module compiled and ready to load.
    kFirmwareMissing, ///< Module available but required firmware blob absent.
    kMissing,       ///< No module found for this device.
    kUnknown,       ///< Could not determine status.
};

const char* driver_status_string(DriverStatus s);

/// Broad hardware category (used for icon/section grouping in OOBE).
enum class HwClass {
    kDisplay,
    kNetwork,
    kAudio,
    kStorage,
    kInput,
    kBluetooth,
    kUsb,
    kThunderbolt,
    kSecurity,  ///< TPM, SGX, SEV
    kPmem,      ///< Persistent memory / NVDIMM
    kRdma,      ///< InfiniBand / RDMA
    kOther,
};

const char* hw_class_string(HwClass c);

/// All information gathered about a single device instance.
struct DeviceInfo {
    std::string id;            ///< sysfs path component, e.g. "0000:03:00.0"
    std::string subsystem;     ///< "pci" | "usb" | "acpi" | "platform"
    HwClass     hw_class = HwClass::kOther;
    std::string class_code;    ///< raw hex, e.g. "0300"
    std::string vendor_name;   ///< "NVIDIA Corporation"
    std::string device_name;   ///< "GA102 [GeForce RTX 3090]"
    std::string modalias;      ///< full modalias string from sysfs
    std::string active_driver; ///< bound driver name (e.g. "nvidia", "i915")
    std::vector<std::string> modules; ///< modules resolved from modalias
    DriverStatus driver_status = DriverStatus::kUnknown;
    std::string firmware_needed; ///< firmware path if kFirmwareMissing
    std::string note;          ///< human-readable status message
};

/// Aggregated result written to /var/lib/straylight/hwdetect.json.
struct HwReport {
    std::string              kernel_version;
    std::string              timestamp_utc;
    std::vector<DeviceInfo>  devices;

    // Counts
    int total    = 0;
    int loaded   = 0;
    int builtin  = 0;
    int missing  = 0;
    int dkms     = 0;
    int firmware_missing = 0;

    std::vector<std::string> dkms_modules; ///< "nvidia/560.35" etc.
};

/// Run full hardware detection pipeline.
///   - Enumerate PCI + USB devices from sysfs.
///   - Resolve modalias → kernel module names.
///   - Check which modules are loaded against /proc/modules.
///   - Attempt modprobe for any unloaded modules.
///   - Validate firmware blobs for wireless/audio devices.
///   - Check DKMS status for out-of-tree drivers.
///   - Return populated report; caller writes JSON.
HwReport detect_all();

/// Write report as JSON to path (atomic: tmp file + rename).
/// Returns true on success.
bool write_report(const HwReport& report, const char* path);

} // namespace straylight::hwdetect
