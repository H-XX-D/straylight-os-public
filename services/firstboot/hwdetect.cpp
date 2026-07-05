// services/firstboot/hwdetect.cpp
// Full hardware detection, module resolution and driver activation for
// StrayLight first-boot.  Reads sysfs directly — no libpci dependency.
//
// Detection pipeline:
//  1. Walk /sys/bus/pci/devices/ and /sys/bus/usb/devices/
//  2. Read modalias, vendor/device IDs, active driver symlink per device.
//  3. Resolve kernel modules via `modprobe --resolve-alias <modalias>`.
//  4. Cross-reference against /proc/modules (loaded modules) and
//     /sys/module/ (built-in modules).
//  5. Run `modprobe <module>` for any unloaded, resolvable driver.
//  6. Validate firmware blobs: check /lib/firmware/ for required files.
//  7. Check `dkms status` for out-of-tree modules (NVIDIA, VirtualBox, etc).
//  8. Emit JSON report to /var/lib/straylight/hwdetect.json.

#include "hwdetect.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs  = std::filesystem;
using json    = nlohmann::json;

namespace straylight::hwdetect {

// ─── helpers ──────────────────────────────────────────────────────────────────

static std::string read_sysfs(const fs::path& p) {
    std::ifstream f(p);
    if (!f.is_open()) return {};
    std::string s;
    std::getline(f, s);
    // Strip trailing whitespace / newlines.
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                           s.back() == ' '))
        s.pop_back();
    return s;
}

/// Run a command and capture stdout.  Returns empty on failure.
static std::string run_capture(const std::string& cmd) {
    std::string out;
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) return out;
    char buf[256];
    while (fgets(buf, sizeof(buf), fp))
        out += buf;
    pclose(fp);
    // Strip trailing newline.
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    return out;
}

/// Run a command and return its exit code.
static int run_silent(const std::string& cmd) {
    std::string full = cmd + " >/dev/null 2>&1";
    return system(full.c_str()); // NOLINT(cert-env33-c)
}

static std::string utc_timestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    return buf;
}

// ─── PCI class code → HwClass mapping ────────────────────────────────────────

static HwClass class_from_code(const std::string& code) {
    // code is decimal or hex string e.g. "196608" (0x30000) or "0300"
    // sysfs "class" file is a hex number like "0x030000"
    uint32_t v = 0;
    try {
        v = static_cast<uint32_t>(std::stoul(code, nullptr, 0));
    } catch (...) {
        return HwClass::kOther;
    }
    uint8_t base = static_cast<uint8_t>((v >> 16) & 0xFF);
    uint8_t sub  = static_cast<uint8_t>((v >> 8)  & 0xFF);
    switch (base) {
        case 0x01: return HwClass::kStorage;
        case 0x02: return (sub == 0x05 || sub == 0x00) ? HwClass::kNetwork
                                                        : HwClass::kNetwork;
        case 0x03: return HwClass::kDisplay;
        case 0x04: return HwClass::kAudio;
        case 0x09: return HwClass::kInput;
        case 0x0C:
            if (sub == 0x03) return HwClass::kUsb;
            if (sub == 0x06) return HwClass::kRdma;
            if (sub == 0x11) return HwClass::kThunderbolt;
            return HwClass::kOther;
        case 0x0D:
            if (sub == 0x11) return HwClass::kBluetooth;
            return HwClass::kNetwork; // WLAN
        case 0x05: return HwClass::kPmem; // Memory controller → may include NVDIMM
        default:   return HwClass::kOther;
    }
}

// ─── /proc/modules and /sys/module/ ──────────────────────────────────────────

static std::unordered_set<std::string> load_proc_modules() {
    std::unordered_set<std::string> mods;
    std::ifstream f("/proc/modules");
    std::string line;
    while (std::getline(f, line)) {
        auto sp = line.find(' ');
        if (sp != std::string::npos) {
            std::string name = line.substr(0, sp);
            // Kernel uses underscores; modalias resolutions use hyphens.
            std::replace(name.begin(), name.end(), '-', '_');
            mods.insert(name);
        }
    }
    return mods;
}

/// Check if a module is compiled in (exists in /sys/module/ but has no srcver
/// because there's no .ko file).  A lighter check: look for /sys/module/<name>.
static bool is_builtin(const std::string& mod) {
    std::string name = mod;
    std::replace(name.begin(), name.end(), '-', '_');
    fs::path p = fs::path("/sys/module") / name;
    if (!fs::exists(p)) return false;
    // A built-in module has the directory but no initstate file, or initstate
    // is "live" with no backing .ko on disk.
    fs::path ko = fs::path("/lib/modules") /
                  run_capture("uname -r") /
                  ("kernel/" + name + ".ko");
    // If sysfs entry exists and there is NO .ko file anywhere in tree, it's
    // built-in.  Use find as fallback.
    std::string found = run_capture(
        "find /lib/modules/$(uname -r) -name '" + name + ".ko*' -maxdepth 6 -print -quit 2>/dev/null");
    return found.empty();
}

// ─── modalias → module resolution ────────────────────────────────────────────

/// Use `modprobe --resolve-alias` to map a modalias to module name(s).
/// Returns vector of module names (usually one, sometimes 0).
static std::vector<std::string> resolve_modalias(const std::string& alias) {
    if (alias.empty()) return {};
    std::string cmd = "modprobe --resolve-alias " +
                      std::string("'") + alias + "' 2>/dev/null";
    std::string out = run_capture(cmd);
    std::vector<std::string> result;
    if (!out.empty()) {
        std::istringstream ss(out);
        std::string mod;
        while (std::getline(ss, mod)) {
            if (!mod.empty()) {
                std::replace(mod.begin(), mod.end(), '-', '_');
                result.push_back(mod);
            }
        }
    }
    return result;
}

// ─── Active driver from sysfs ─────────────────────────────────────────────────

static std::string active_driver(const fs::path& dev_path) {
    // /sys/bus/pci/devices/<id>/driver → symlink like ../../bus/pci/drivers/i915
    fs::path drv = dev_path / "driver";
    if (fs::exists(drv) && fs::is_symlink(drv)) {
        return fs::read_symlink(drv).filename().string();
    }
    return {};
}

// ─── firmware validation ──────────────────────────────────────────────────────

/// Known firmware blobs required by common wireless/other drivers.
/// Format: module_name -> list of firmware paths to check under /lib/firmware/.
static const std::unordered_map<std::string, std::vector<std::string>>
    kFirmwareMap = {
    // Intel WiFi (iwlwifi)
    {"iwlwifi", {
        "iwlwifi-cc-a0-72.ucode",
        "iwlwifi-QuZ-a0-hr-b0-72.ucode",
        "iwlwifi-so-a0-hr-b0-72.ucode",
        "iwlwifi-ty-a0-gf-a0-72.ucode",
        "iwlwifi-Qu-c0-hr-b0-72.ucode",
    }},
    // Realtek WiFi (rtw89)
    {"rtw89_8852be", {"rtw89/rtw8852b_fw.bin"}},
    {"rtw89_8922ae", {"rtw89/rtw8922a_fw.bin"}},
    // Realtek 8723de/88x2bu
    {"rtl8723de", {"rtlwifi/rtl8723defw.bin"}},
    // Broadcom WiFi
    {"brcmfmac", {
        "brcm/brcmfmac4356-pcie.bin",
        "brcm/brcmfmac4371-pcie.bin",
        "brcm/brcmfmac4364-pcie.apple,tahoe.bin",
    }},
    // AMD GPU microcode
    {"amdgpu", {
        "amdgpu/navi31_me.bin",
        "amdgpu/navi31_pfp.bin",
    }},
    // Intel GPU GuC/HuC
    {"i915", {
        "i915/tgl_guc_70.bin",
        "i915/adls_guc_70.bin",
        "i915/dg2_guc_70.bin",
    }},
    // Bluetooth firmware
    {"btusb",   {"intel/ibt-1040-4150.sfi", "intel/ibt-hw-37.8.10-fw-22.50.19.14.f.bseq"}},
    {"btintel", {"intel/ibt-0040-1050.sfi"}},
};

static std::string check_firmware(const std::string& mod) {
    auto it = kFirmwareMap.find(mod);
    if (it == kFirmwareMap.end()) return {};
    for (const auto& fw : it->second) {
        if (fs::exists(fs::path("/lib/firmware") / fw)) {
            return {}; // at least one found — OK
        }
    }
    // None of the known blobs exist — return the first expected path.
    return it->second.empty() ? std::string{} : it->second.front();
}

// ─── DKMS ─────────────────────────────────────────────────────────────────────

struct DkmsEntry {
    std::string name;
    std::string version;
    std::string status; // "installed", "built", "added", "building"
};

static std::vector<DkmsEntry> query_dkms() {
    std::vector<DkmsEntry> results;
    // `dkms status` outputs: <name>/<version>, <kernel>/<arch>: installed
    std::string out = run_capture("dkms status 2>/dev/null");
    if (out.empty()) return results;
    std::istringstream ss(out);
    std::string line;
    while (std::getline(ss, line)) {
        // Parse "nvidia/560.35.03, 6.8.0-51-generic/x86_64: installed"
        auto comma = line.find(',');
        auto slash = line.find('/');
        if (slash == std::string::npos) continue;
        DkmsEntry e;
        e.name    = line.substr(0, slash);
        size_t end = (comma != std::string::npos) ? comma : line.size();
        e.version = line.substr(slash + 1, end - slash - 1);
        auto colon = line.find(':');
        if (colon != std::string::npos) {
            e.status = line.substr(colon + 2);
            // trim trailing whitespace
            while (!e.status.empty() && e.status.back() == ' ')
                e.status.pop_back();
        }
        results.push_back(std::move(e));
    }
    return results;
}

// ─── Vendor/device name lookup ────────────────────────────────────────────────

/// Read a 4-char hex string from sysfs (vendor/device files).
static std::string read_hex_id(const fs::path& p) {
    std::string s = read_sysfs(p);
    // sysfs prefixes with "0x"
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s = s.substr(2);
    // Lowercase
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

/// Best-effort lookup from /usr/share/misc/pci.ids (hwdata) or /usr/share/hwdata/pci.ids.
/// Returns "Vendor Description Device Description" or empty string.
static std::pair<std::string,std::string> lookup_pci_ids(
    const std::string& vendor, const std::string& device)
{
    static std::once_flag flag;
    static std::unordered_map<std::string,std::string> vendor_names;
    static std::unordered_map<std::string,std::string> device_names;

    // Lazy-load pci.ids once
    std::call_once(flag, [&]() {
        const char* paths[] = {
            "/usr/share/misc/pci.ids",
            "/usr/share/hwdata/pci.ids",
            "/usr/share/pci.ids",
            nullptr
        };
        for (int i = 0; paths[i]; ++i) {
            std::ifstream f(paths[i]);
            if (!f.is_open()) continue;
            std::string line;
            std::string cur_vendor;
            while (std::getline(f, line)) {
                if (line.empty() || line[0] == '#') continue;
                if (line[0] == '\t' && !cur_vendor.empty()) {
                    if (line.size() >= 5 && line[1] != '\t') {
                        // Device line: "\t<id>  <name>"
                        std::string dev_id = line.substr(1, 4);
                        auto sp = line.find("  ", 5);
                        std::string dev_name = (sp != std::string::npos)
                            ? line.substr(sp + 2) : line.substr(6);
                        device_names[cur_vendor + ":" + dev_id] = dev_name;
                    }
                } else if (line[0] != '\t' && line.size() >= 6) {
                    cur_vendor = line.substr(0, 4);
                    auto sp = line.find("  ", 4);
                    std::string vname = (sp != std::string::npos)
                        ? line.substr(sp + 2) : line.substr(6);
                    vendor_names[cur_vendor] = vname;
                }
            }
            break;
        }
    });

    std::string vname, dname;
    auto vit = vendor_names.find(vendor);
    if (vit != vendor_names.end()) vname = vit->second;
    auto dit = device_names.find(vendor + ":" + device);
    if (dit != device_names.end()) dname = dit->second;
    return {vname, dname};
}

// ─── PCI enumeration ──────────────────────────────────────────────────────────

static std::vector<DeviceInfo> enumerate_pci(
    const std::unordered_set<std::string>& loaded_modules)
{
    std::vector<DeviceInfo> devices;
    fs::path bus("/sys/bus/pci/devices");
    if (!fs::exists(bus)) return devices;

    for (const auto& entry : fs::directory_iterator(bus)) {
        DeviceInfo di;
        di.subsystem = "pci";
        di.id        = entry.path().filename().string();

        // Class code (hex, e.g. "0x030200")
        std::string class_raw = read_sysfs(entry.path() / "class");
        di.class_code = class_raw;
        di.hw_class   = class_from_code(class_raw);

        // Skip PCI bridges, root complexes, host bridges by class
        uint32_t class_val = 0;
        try { class_val = std::stoul(class_raw, nullptr, 0); } catch(...) {}
        uint8_t base_class = static_cast<uint8_t>((class_val >> 16) & 0xFF);
        if (base_class == 0x06 || base_class == 0x08) continue; // bridges/generic

        // Vendor + device IDs
        std::string vid = read_hex_id(entry.path() / "vendor");
        std::string did = read_hex_id(entry.path() / "device");
        auto [vname, dname] = lookup_pci_ids(vid, did);
        di.vendor_name = vname.empty() ? ("PCI:" + vid) : vname;
        di.device_name = dname.empty() ? did : dname;

        // Modalias
        di.modalias = read_sysfs(entry.path() / "modalias");

        // Active driver
        di.active_driver = active_driver(entry.path());

        // Module resolution
        if (!di.active_driver.empty()) {
            di.modules = {di.active_driver};
            std::string norm = di.active_driver;
            std::replace(norm.begin(), norm.end(), '-', '_');
            if (loaded_modules.count(norm) || is_builtin(norm)) {
                di.driver_status = (is_builtin(norm))
                    ? DriverStatus::kBuiltin : DriverStatus::kLoaded;
                di.note = "Driver active: " + di.active_driver;
            } else {
                di.driver_status = DriverStatus::kUnknown;
                di.note = "Driver bound but module not in /proc/modules";
            }
        } else {
            // No driver bound — resolve via modalias
            di.modules = resolve_modalias(di.modalias);
            if (di.modules.empty()) {
                // No module found for this modalias
                di.driver_status = DriverStatus::kMissing;
                di.note = "No kernel module found for " + di.modalias;
            } else {
                // Check if already loaded or builtin
                bool any_loaded = false;
                for (const auto& m : di.modules) {
                    if (loaded_modules.count(m) || is_builtin(m)) {
                        any_loaded = true;
                        di.driver_status = is_builtin(m)
                            ? DriverStatus::kBuiltin : DriverStatus::kLoaded;
                        di.note = "Module loaded: " + m;
                        break;
                    }
                }
                if (!any_loaded) {
                    // Try modprobe
                    for (const auto& m : di.modules) {
                        std::string fw_missing = check_firmware(m);
                        if (!fw_missing.empty()) {
                            di.driver_status  = DriverStatus::kFirmwareMissing;
                            di.firmware_needed = fw_missing;
                            di.note = "Module " + m +
                                      " needs firmware: " + fw_missing;
                            break;
                        }
                        int rc = run_silent("modprobe " + m);
                        if (rc == 0) {
                            di.driver_status = DriverStatus::kLoaded;
                            di.active_driver = m;
                            di.note = "Loaded via modprobe: " + m;
                            break;
                        }
                    }
                    if (di.driver_status == DriverStatus::kUnknown) {
                        di.driver_status = DriverStatus::kMissing;
                        di.note = "modprobe failed for: " + di.modules.front();
                    }
                }
            }
        }

        // Firmware check for loaded wireless/GPU drivers
        if ((di.driver_status == DriverStatus::kLoaded ||
             di.driver_status == DriverStatus::kBuiltin) &&
            !di.active_driver.empty()) {
            std::string fw = check_firmware(di.active_driver);
            if (!fw.empty()) {
                di.driver_status  = DriverStatus::kFirmwareMissing;
                di.firmware_needed = fw;
                di.note = "Firmware missing: " + fw;
            }
        }

        devices.push_back(std::move(di));
    }
    return devices;
}

// ─── USB enumeration ──────────────────────────────────────────────────────────

static std::vector<DeviceInfo> enumerate_usb(
    const std::unordered_set<std::string>& loaded_modules)
{
    std::vector<DeviceInfo> devices;
    fs::path bus("/sys/bus/usb/devices");
    if (!fs::exists(bus)) return devices;

    for (const auto& entry : fs::directory_iterator(bus)) {
        // Skip USB hubs and root hubs (no idVendor)
        if (!fs::exists(entry.path() / "idVendor")) continue;

        DeviceInfo di;
        di.subsystem = "usb";
        di.id        = entry.path().filename().string();

        std::string vid = read_sysfs(entry.path() / "idVendor");
        std::string did = read_sysfs(entry.path() / "idProduct");
        di.vendor_name = read_sysfs(entry.path() / "manufacturer");
        if (di.vendor_name.empty()) di.vendor_name = "USB:" + vid;
        di.device_name = read_sysfs(entry.path() / "product");
        if (di.device_name.empty()) di.device_name = did;
        di.modalias    = read_sysfs(entry.path() / "modalias");

        // USB class
        std::string bclass = read_sysfs(entry.path() / "bDeviceClass");
        uint32_t uclass = 0;
        try { uclass = std::stoul(bclass, nullptr, 16); } catch (...) {}
        switch (uclass) {
            case 0x01: di.hw_class = HwClass::kAudio;     break;
            case 0x02: di.hw_class = HwClass::kNetwork;   break;
            case 0x03: di.hw_class = HwClass::kInput;     break;
            case 0x07: di.hw_class = HwClass::kOther;     break; // Printer
            case 0x08: di.hw_class = HwClass::kStorage;   break;
            case 0xE0: di.hw_class = HwClass::kBluetooth; break;
            default:   di.hw_class = HwClass::kUsb;       break;
        }

        di.active_driver = active_driver(entry.path());

        if (!di.active_driver.empty()) {
            std::string norm = di.active_driver;
            std::replace(norm.begin(), norm.end(), '-', '_');
            di.modules       = {di.active_driver};
            di.driver_status = loaded_modules.count(norm)
                ? DriverStatus::kLoaded : DriverStatus::kUnknown;
            di.note = "Driver: " + di.active_driver;
        } else {
            di.modules = resolve_modalias(di.modalias);
            if (di.modules.empty()) {
                di.driver_status = DriverStatus::kMissing;
                di.note = "No module for USB device " + vid + ":" + did;
            } else {
                bool loaded = false;
                for (const auto& m : di.modules) {
                    if (loaded_modules.count(m) || is_builtin(m)) {
                        loaded = true;
                        di.driver_status = is_builtin(m)
                            ? DriverStatus::kBuiltin : DriverStatus::kLoaded;
                        di.note = "Module loaded: " + m;
                        break;
                    }
                }
                if (!loaded) {
                    for (const auto& m : di.modules) {
                        if (run_silent("modprobe " + m) == 0) {
                            di.driver_status = DriverStatus::kLoaded;
                            di.active_driver = m;
                            di.note = "Loaded via modprobe: " + m;
                            break;
                        }
                    }
                    if (di.driver_status == DriverStatus::kUnknown) {
                        di.driver_status = DriverStatus::kMissing;
                        di.note = "modprobe failed";
                    }
                }
            }
        }

        devices.push_back(std::move(di));
    }
    return devices;
}

// ─── DKMS crosscheck ──────────────────────────────────────────────────────────

/// Mark any kMissing device whose module has a DKMS entry as kDkmsReady
/// or kDkmsBuilding.
static void apply_dkms(std::vector<DeviceInfo>& devices,
                        const std::vector<DkmsEntry>& dkms_list,
                        std::vector<std::string>& out_dkms_modules)
{
    for (auto& di : devices) {
        for (const auto& mod : di.modules) {
            for (const auto& dkms : dkms_list) {
                // Match module name by convention: nvidia dkms → "nvidia" mod
                std::string dname = dkms.name;
                std::transform(dname.begin(), dname.end(), dname.begin(),
                               [](unsigned char c){ return std::tolower(c); });
                std::string mname = mod;
                std::transform(mname.begin(), mname.end(), mname.begin(),
                               [](unsigned char c){ return std::tolower(c); });
                if (mname.find(dname) != std::string::npos ||
                    dname.find(mname) != std::string::npos)
                {
                    std::string slug = dkms.name + "/" + dkms.version;
                    out_dkms_modules.push_back(slug);
                    if (dkms.status == "installed") {
                        di.driver_status = DriverStatus::kDkmsReady;
                        di.note = "DKMS installed: " + slug;
                    } else if (dkms.status == "building" ||
                               dkms.status == "added") {
                        di.driver_status = DriverStatus::kDkmsBuilding;
                        di.note = "DKMS building: " + slug;
                    }
                }
            }
        }
    }
}

// ─── Public API ───────────────────────────────────────────────────────────────

const char* driver_status_string(DriverStatus s) {
    switch (s) {
        case DriverStatus::kLoaded:          return "loaded";
        case DriverStatus::kBuiltin:         return "builtin";
        case DriverStatus::kDkmsBuilding:    return "dkms_building";
        case DriverStatus::kDkmsReady:       return "dkms_ready";
        case DriverStatus::kFirmwareMissing: return "firmware_missing";
        case DriverStatus::kMissing:         return "missing";
        case DriverStatus::kUnknown:         return "unknown";
    }
    return "unknown";
}

const char* hw_class_string(HwClass c) {
    switch (c) {
        case HwClass::kDisplay:     return "Display";
        case HwClass::kNetwork:     return "Network";
        case HwClass::kAudio:       return "Audio";
        case HwClass::kStorage:     return "Storage";
        case HwClass::kInput:       return "Input";
        case HwClass::kBluetooth:   return "Bluetooth";
        case HwClass::kUsb:         return "USB";
        case HwClass::kThunderbolt: return "Thunderbolt";
        case HwClass::kSecurity:    return "Security";
        case HwClass::kPmem:        return "PersistentMemory";
        case HwClass::kRdma:        return "RDMA";
        case HwClass::kOther:       return "Other";
    }
    return "Other";
}

HwReport detect_all() {
    HwReport report;
    report.timestamp_utc  = utc_timestamp();
    report.kernel_version = run_capture("uname -r");

    auto loaded_mods = load_proc_modules();

    auto pci_devs = enumerate_pci(loaded_mods);
    auto usb_devs = enumerate_usb(loaded_mods);

    report.devices.insert(report.devices.end(), pci_devs.begin(), pci_devs.end());
    report.devices.insert(report.devices.end(), usb_devs.begin(), usb_devs.end());

    // DKMS cross-check
    auto dkms_list = query_dkms();
    apply_dkms(report.devices, dkms_list, report.dkms_modules);

    // Deduplicate dkms_modules
    std::sort(report.dkms_modules.begin(), report.dkms_modules.end());
    report.dkms_modules.erase(
        std::unique(report.dkms_modules.begin(), report.dkms_modules.end()),
        report.dkms_modules.end());

    // Compute summary
    for (const auto& di : report.devices) {
        report.total++;
        switch (di.driver_status) {
            case DriverStatus::kLoaded:          report.loaded++;   break;
            case DriverStatus::kBuiltin:         report.builtin++;  break;
            case DriverStatus::kDkmsBuilding:    report.dkms++;     break;
            case DriverStatus::kDkmsReady:       report.loaded++;   break;
            case DriverStatus::kFirmwareMissing: report.firmware_missing++; break;
            case DriverStatus::kMissing:         report.missing++;  break;
            case DriverStatus::kUnknown:         report.missing++;  break;
        }
    }

    return report;
}

bool write_report(const HwReport& report, const char* path) {
    json j;
    j["timestamp"]      = report.timestamp_utc;
    j["kernel_version"] = report.kernel_version;
    j["dkms_modules"]   = report.dkms_modules;
    j["summary"] = {
        {"total",            report.total},
        {"loaded",           report.loaded},
        {"builtin",          report.builtin},
        {"missing",          report.missing},
        {"dkms",             report.dkms},
        {"firmware_missing", report.firmware_missing},
    };

    json devs = json::array();
    for (const auto& di : report.devices) {
        json d;
        d["id"]              = di.id;
        d["subsystem"]       = di.subsystem;
        d["hw_class"]        = hw_class_string(di.hw_class);
        d["class_code"]      = di.class_code;
        d["vendor"]          = di.vendor_name;
        d["device"]          = di.device_name;
        d["modalias"]        = di.modalias;
        d["active_driver"]   = di.active_driver;
        d["driver_status"]   = driver_status_string(di.driver_status);
        d["modules"]         = di.modules;
        d["firmware_needed"] = di.firmware_needed;
        d["note"]            = di.note;
        devs.push_back(std::move(d));
    }
    j["devices"] = std::move(devs);

    std::string tmp = std::string(path) + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f.is_open()) return false;
        f << j.dump(2) << "\n";
        f.flush();
    }
    // fsync + atomic rename
    {
        FILE* fp = fopen(tmp.c_str(), "r");
        if (fp) { fsync(fileno(fp)); fclose(fp); }
    }
    return rename(tmp.c_str(), path) == 0;
}

} // namespace straylight::hwdetect
