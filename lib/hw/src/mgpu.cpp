#include <straylight/hw/mgpu.h>
#include <straylight/log.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <sys/ioctl.h>
#include <unistd.h>

/* --------------------------------------------------------------------------
 * Kernel ioctl structures — mirrored from kernel/vpu/vpu_mgpu.h
 * These must stay in sync with the kernel definitions.
 * -------------------------------------------------------------------------- */

#define VPU_MGPU_MAX_GPUS  16
#define VPU_IOC_MAGIC      'V'

namespace {

struct k_vpu_gpu_info {
    uint32_t index;
    uint16_t pci_vendor;
    uint16_t pci_device;
    uint64_t vram_total;
    uint64_t vram_used;
    uint64_t bar0_addr;
    uint64_t bar0_size;
    uint64_t bar2_addr;
    uint64_t bar2_size;
    uint32_t link_speed;
    uint32_t link_width;
    int32_t  temperature;
    uint32_t p2p_peers;
    char     pci_slot[16];
    uint32_t pad;
};

struct k_vpu_enum_gpus_req {
    uint32_t count;
    uint32_t pad;
    k_vpu_gpu_info gpus[VPU_MGPU_MAX_GPUS];
};

struct k_vpu_alloc_on_req {
    uint32_t gpu_index;
    uint32_t flags;
    uint64_t size;
    uint64_t handle;
    uint64_t gpu_addr;
};

struct k_vpu_p2p_copy_req {
    uint64_t src_handle;
    uint64_t dst_handle;
    uint64_t size;
    uint64_t offset;
};

struct k_vpu_mirror_req {
    uint64_t src_handle;
    uint32_t gpu_mask;
    uint32_t count;
    uint64_t mirror_handles[VPU_MGPU_MAX_GPUS];
};

struct k_vpu_gpu_stats_req {
    uint32_t gpu_index;
    uint32_t pad;
    k_vpu_gpu_info info;
};

// ioctl numbers matching kernel definitions
#define VPU_IOC_ENUM_GPUS   _IOR (VPU_IOC_MAGIC, 0x10, k_vpu_enum_gpus_req)
#define VPU_IOC_ALLOC_ON    _IOWR(VPU_IOC_MAGIC, 0x11, k_vpu_alloc_on_req)
#define VPU_IOC_P2P_COPY    _IOW (VPU_IOC_MAGIC, 0x12, k_vpu_p2p_copy_req)
#define VPU_IOC_MIRROR      _IOWR(VPU_IOC_MAGIC, 0x13, k_vpu_mirror_req)
#define VPU_IOC_GPU_STATS   _IOWR(VPU_IOC_MAGIC, 0x14, k_vpu_gpu_stats_req)

/* --------------------------------------------------------------------------
 * PCI vendor name/device name heuristics
 * -------------------------------------------------------------------------- */

const char* vendor_string(uint16_t vendor) {
    switch (vendor) {
        case 0x10de: return "nvidia";
        case 0x1002: return "amd";
        case 0x8086: return "intel";
        default:     return "unknown";
    }
}

std::string device_name(uint16_t vendor, uint16_t device) {
    std::string prefix;
    switch (vendor) {
        case 0x10de: prefix = "NVIDIA GPU"; break;
        case 0x1002: prefix = "AMD GPU"; break;
        case 0x8086: prefix = "Intel GPU"; break;
        default:     prefix = "Unknown GPU"; break;
    }

    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s [%04x:%04x]", prefix.c_str(),
                  vendor, device);
    return std::string(buf);
}

/* --------------------------------------------------------------------------
 * sysfs PCI bus fallback scanner
 * -------------------------------------------------------------------------- */

straylight::hw::GpuDeviceInfo make_info_from_sysfs(const std::string& slot_path,
                                                    uint32_t index) {
    straylight::hw::GpuDeviceInfo info{};
    info.index = index;
    info.temperature = -1.0f;
    info.p2p_peers = 0;

    // Read vendor
    {
        std::ifstream f(slot_path + "/vendor");
        unsigned int v = 0;
        if (f >> std::hex >> v) {
            info.pci_vendor = static_cast<uint16_t>(v);
            info.vendor = vendor_string(info.pci_vendor);
        }
    }

    // Read device
    {
        std::ifstream f(slot_path + "/device");
        unsigned int d = 0;
        if (f >> std::hex >> d) {
            info.pci_device = static_cast<uint16_t>(d);
        }
    }

    info.name = device_name(info.pci_vendor, info.pci_device);

    // Read resource (BARs) to estimate VRAM
    {
        std::ifstream f(slot_path + "/resource");
        std::string line;
        size_t max_bar = 0;
        while (std::getline(f, line)) {
            unsigned long long start = 0, end = 0, flags = 0;
            if (std::sscanf(line.c_str(), "%llx %llx %llx",
                            &start, &end, &flags) == 3) {
                if (end > start) {
                    size_t sz = static_cast<size_t>(end - start + 1);
                    if (sz > max_bar)
                        max_bar = sz;
                }
            }
        }
        info.vram_total = max_bar;
    }

    info.vram_used = 0;

    // Extract PCI slot from path
    {
        auto pos = slot_path.rfind('/');
        if (pos != std::string::npos)
            info.pci_slot = slot_path.substr(pos + 1);
    }

    return info;
}

bool is_gpu_class(const std::string& slot_path) {
    std::ifstream f(slot_path + "/class");
    unsigned int cls = 0;
    if (!(f >> std::hex >> cls))
        return false;

    // VGA controller: 0x030000, 3D controller: 0x030200
    unsigned int base = (cls >> 8) & 0xFFFF;
    return (base == 0x0300 || base == 0x0302);
}

bool is_supported_vendor(uint16_t vendor) {
    return vendor == 0x10de || vendor == 0x1002 || vendor == 0x8086;
}

std::vector<straylight::hw::GpuDeviceInfo> scan_sysfs_pci() {
    std::vector<straylight::hw::GpuDeviceInfo> result;
    const char* pci_path = "/sys/bus/pci/devices";

    DIR* dir = opendir(pci_path);
    if (!dir)
        return result;

    struct dirent* ent;
    uint32_t idx = 0;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] == '.')
            continue;

        std::string slot_path = std::string(pci_path) + "/" + ent->d_name;

        if (!is_gpu_class(slot_path))
            continue;

        auto info = make_info_from_sysfs(slot_path, idx);
        if (!is_supported_vendor(info.pci_vendor))
            continue;

        result.push_back(std::move(info));
        idx++;
    }

    closedir(dir);
    return result;
}

/* --------------------------------------------------------------------------
 * Read a single sysfs attribute as a string
 * -------------------------------------------------------------------------- */

std::string read_sysfs_attr(const std::string& path) {
    std::ifstream f(path);
    std::string val;
    if (std::getline(f, val))
        return val;
    return "";
}

straylight::hw::GpuDeviceInfo read_gpu_sysfs(uint32_t gpu_index) {
    straylight::hw::GpuDeviceInfo info{};
    info.index = gpu_index;
    info.temperature = -1.0f;

    char base[128];
    std::snprintf(base, sizeof(base),
                  "/sys/kernel/straylight-vpu/gpu%u", gpu_index);

    std::string s;

    s = read_sysfs_attr(std::string(base) + "/vendor");
    if (!s.empty()) {
        unsigned int v = 0;
        std::sscanf(s.c_str(), "%x", &v);
        info.pci_vendor = static_cast<uint16_t>(v);
        info.vendor = vendor_string(info.pci_vendor);
    }

    s = read_sysfs_attr(std::string(base) + "/device");
    if (!s.empty()) {
        unsigned int d = 0;
        std::sscanf(s.c_str(), "%x", &d);
        info.pci_device = static_cast<uint16_t>(d);
    }

    info.name = device_name(info.pci_vendor, info.pci_device);

    s = read_sysfs_attr(std::string(base) + "/vram_total");
    if (!s.empty())
        info.vram_total = std::stoull(s);

    s = read_sysfs_attr(std::string(base) + "/vram_used");
    if (!s.empty())
        info.vram_used = std::stoull(s);

    s = read_sysfs_attr(std::string(base) + "/temperature");
    if (!s.empty()) {
        int milli = std::stoi(s);
        if (milli < 0)
            info.temperature = -1.0f;
        else
            info.temperature = static_cast<float>(milli) / 1000.0f;
    }

    s = read_sysfs_attr(std::string(base) + "/p2p_peers");
    if (!s.empty()) {
        unsigned int p = 0;
        std::sscanf(s.c_str(), "%x", &p);
        info.p2p_peers = static_cast<int>(p);
    }

    s = read_sysfs_attr(std::string(base) + "/link_speed");
    if (!s.empty()) {
        /* link_speed stored but not in GpuDeviceInfo — we use pci_slot */
    }

    return info;
}

} // anonymous namespace

/* ========================================================================== */

namespace straylight::hw {

/* --------------------------------------------------------------------------
 * Construction / Destruction
 * -------------------------------------------------------------------------- */

MultiGpuManager::MultiGpuManager() = default;

MultiGpuManager::~MultiGpuManager() {
    if (vpu_fd_ >= 0) {
        ::close(vpu_fd_);
        vpu_fd_ = -1;
    }
}

MultiGpuManager::MultiGpuManager(MultiGpuManager&& other) noexcept
    : vpu_fd_(other.vpu_fd_),
      gpus_(std::move(other.gpus_)),
      rr_counter_(other.rr_counter_),
      last_gpu_(other.last_gpu_) {
    other.vpu_fd_ = -1;
}

MultiGpuManager& MultiGpuManager::operator=(MultiGpuManager&& other) noexcept {
    if (this != &other) {
        if (vpu_fd_ >= 0)
            ::close(vpu_fd_);
        vpu_fd_ = other.vpu_fd_;
        gpus_ = std::move(other.gpus_);
        rr_counter_ = other.rr_counter_;
        last_gpu_ = other.last_gpu_;
        other.vpu_fd_ = -1;
    }
    return *this;
}

/* --------------------------------------------------------------------------
 * Discovery
 * -------------------------------------------------------------------------- */

Result<void, std::string> MultiGpuManager::discover() {
    std::lock_guard lock(mu_);

    gpus_.clear();
    rr_counter_ = 0;
    last_gpu_ = 0;

    // Try to open the VPU device
    if (vpu_fd_ < 0) {
        vpu_fd_ = ::open("/dev/straylight-vpu", O_RDWR | O_CLOEXEC);
    }

    // Attempt ioctl-based discovery
    if (vpu_fd_ >= 0) {
        k_vpu_enum_gpus_req req{};
        int ret = ::ioctl(vpu_fd_, VPU_IOC_ENUM_GPUS, &req);
        if (ret == 0 && req.count > 0) {
            for (uint32_t i = 0; i < req.count; i++) {
                const auto& kg = req.gpus[i];
                GpuDeviceInfo info{};
                info.index = kg.index;
                info.pci_vendor = kg.pci_vendor;
                info.pci_device = kg.pci_device;
                info.vendor = vendor_string(kg.pci_vendor);
                info.name = device_name(kg.pci_vendor, kg.pci_device);
                info.vram_total = static_cast<size_t>(kg.vram_total);
                info.vram_used = static_cast<size_t>(kg.vram_used);
                info.p2p_peers = static_cast<int>(kg.p2p_peers);
                info.pci_slot = std::string(kg.pci_slot,
                    strnlen(kg.pci_slot, sizeof(kg.pci_slot)));

                if (kg.temperature < 0)
                    info.temperature = -1.0f;
                else
                    info.temperature =
                        static_cast<float>(kg.temperature) / 1000.0f;

                gpus_.push_back(std::move(info));
            }

            SL_DEBUG("MultiGpuManager: discovered {} GPUs via ioctl",
                     gpus_.size());
            return Result<void, std::string>::ok();
        }
    }

    // Fallback: scan sysfs PCI bus directly
    gpus_ = scan_sysfs_pci();

    if (gpus_.empty()) {
        SL_DEBUG("MultiGpuManager: no GPUs discovered");
        return Result<void, std::string>::ok();
    }

    SL_DEBUG("MultiGpuManager: discovered {} GPUs via sysfs fallback",
             gpus_.size());
    return Result<void, std::string>::ok();
}

/* --------------------------------------------------------------------------
 * Accessors
 * -------------------------------------------------------------------------- */

std::vector<GpuDeviceInfo> MultiGpuManager::gpus() const {
    std::lock_guard lock(mu_);
    return gpus_;
}

size_t MultiGpuManager::gpu_count() const {
    std::lock_guard lock(mu_);
    return gpus_.size();
}

/* --------------------------------------------------------------------------
 * Policy-driven allocation
 * -------------------------------------------------------------------------- */

Result<MgpuAllocation, std::string> MultiGpuManager::allocate(
    size_t bytes, PlacementPolicy policy) {
    std::lock_guard lock(mu_);

    if (gpus_.empty())
        return Result<MgpuAllocation, std::string>::error(
            "No GPUs available");

    if (bytes == 0)
        return Result<MgpuAllocation, std::string>::error(
            "Cannot allocate 0 bytes");

    uint32_t target = 0;

    switch (policy) {
    case PlacementPolicy::RoundRobin:
        target = rr_counter_ % static_cast<uint32_t>(gpus_.size());
        rr_counter_++;
        break;

    case PlacementPolicy::LeastUsed: {
        size_t min_used = SIZE_MAX;
        for (size_t i = 0; i < gpus_.size(); i++) {
            if (gpus_[i].vram_used < min_used) {
                min_used = gpus_[i].vram_used;
                target = static_cast<uint32_t>(i);
            }
        }
        break;
    }

    case PlacementPolicy::Affinity:
        target = last_gpu_;
        if (target >= static_cast<uint32_t>(gpus_.size()))
            target = 0;
        break;

    case PlacementPolicy::Mirror: {
        /* Allocate on GPU 0 first, then mirror to all others */
        /* We need to unlock mu_ since allocate_on and mirror
         * also need the lock — but we already hold it here.
         * Work with the fd directly instead. */
        if (vpu_fd_ < 0)
            return Result<MgpuAllocation, std::string>::error(
                "VPU device not open for mirror allocation");

        k_vpu_alloc_on_req areq{};
        areq.gpu_index = 0;
        areq.flags = 0;
        areq.size = static_cast<uint64_t>(bytes);

        int ret = ::ioctl(vpu_fd_, VPU_IOC_ALLOC_ON, &areq);
        if (ret < 0)
            return Result<MgpuAllocation, std::string>::error(
                "Mirror primary allocation failed: " +
                std::string(strerror(errno)));

        uint32_t all_mask = 0;
        for (size_t i = 0; i < gpus_.size(); i++)
            all_mask |= (1U << i);

        k_vpu_mirror_req mreq{};
        mreq.src_handle = areq.handle;
        mreq.gpu_mask = all_mask;

        ret = ::ioctl(vpu_fd_, VPU_IOC_MIRROR, &mreq);
        if (ret < 0)
            return Result<MgpuAllocation, std::string>::error(
                "Mirror ioctl failed: " +
                std::string(strerror(errno)));

        MgpuAllocation alloc{};
        alloc.handle = areq.handle;
        alloc.gpu_index = 0;
        alloc.gpu_addr = areq.gpu_addr;
        alloc.size = bytes;
        for (uint32_t i = 0; i < mreq.count; i++)
            alloc.mirror_handles.push_back(mreq.mirror_handles[i]);

        last_gpu_ = 0;
        gpus_[0].vram_used += bytes;

        return Result<MgpuAllocation, std::string>::ok(std::move(alloc));
    }
    }

    // Normal (non-mirror) allocation via ioctl
    if (vpu_fd_ >= 0) {
        k_vpu_alloc_on_req req{};
        req.gpu_index = target;
        req.flags = 0;
        req.size = static_cast<uint64_t>(bytes);

        int ret = ::ioctl(vpu_fd_, VPU_IOC_ALLOC_ON, &req);
        if (ret < 0)
            return Result<MgpuAllocation, std::string>::error(
                "ALLOC_ON ioctl failed: " +
                std::string(strerror(errno)));

        MgpuAllocation alloc{};
        alloc.handle = req.handle;
        alloc.gpu_index = target;
        alloc.gpu_addr = req.gpu_addr;
        alloc.size = bytes;

        last_gpu_ = target;
        if (target < gpus_.size())
            gpus_[target].vram_used += bytes;

        return Result<MgpuAllocation, std::string>::ok(std::move(alloc));
    }

    // No device — return a synthetic allocation for testing
    MgpuAllocation alloc{};
    alloc.handle = static_cast<uint64_t>(target) << 48 |
                   static_cast<uint64_t>(rr_counter_);
    alloc.gpu_index = target;
    alloc.gpu_addr = 0;
    alloc.size = bytes;

    last_gpu_ = target;
    if (target < gpus_.size())
        gpus_[target].vram_used += bytes;

    return Result<MgpuAllocation, std::string>::ok(std::move(alloc));
}

/* --------------------------------------------------------------------------
 * Targeted allocation
 * -------------------------------------------------------------------------- */

Result<MgpuAllocation, std::string> MultiGpuManager::allocate_on(
    uint32_t gpu_index, size_t bytes) {
    std::lock_guard lock(mu_);

    if (gpu_index >= static_cast<uint32_t>(gpus_.size()))
        return Result<MgpuAllocation, std::string>::error(
            "GPU index " + std::to_string(gpu_index) + " out of range");

    if (bytes == 0)
        return Result<MgpuAllocation, std::string>::error(
            "Cannot allocate 0 bytes");

    if (vpu_fd_ >= 0) {
        k_vpu_alloc_on_req req{};
        req.gpu_index = gpu_index;
        req.flags = 0;
        req.size = static_cast<uint64_t>(bytes);

        int ret = ::ioctl(vpu_fd_, VPU_IOC_ALLOC_ON, &req);
        if (ret < 0)
            return Result<MgpuAllocation, std::string>::error(
                "ALLOC_ON ioctl failed: " +
                std::string(strerror(errno)));

        MgpuAllocation alloc{};
        alloc.handle = req.handle;
        alloc.gpu_index = gpu_index;
        alloc.gpu_addr = req.gpu_addr;
        alloc.size = bytes;

        last_gpu_ = gpu_index;
        gpus_[gpu_index].vram_used += bytes;

        return Result<MgpuAllocation, std::string>::ok(std::move(alloc));
    }

    // No device — synthetic allocation
    MgpuAllocation alloc{};
    alloc.handle = static_cast<uint64_t>(gpu_index) << 48 |
                   static_cast<uint64_t>(++rr_counter_);
    alloc.gpu_index = gpu_index;
    alloc.gpu_addr = 0;
    alloc.size = bytes;

    last_gpu_ = gpu_index;
    gpus_[gpu_index].vram_used += bytes;

    return Result<MgpuAllocation, std::string>::ok(std::move(alloc));
}

/* --------------------------------------------------------------------------
 * Free
 * -------------------------------------------------------------------------- */

Result<void, std::string> MultiGpuManager::free(const MgpuAllocation& alloc) {
    std::lock_guard lock(mu_);

    if (vpu_fd_ >= 0) {
        /* Use the basic VPU_IOC_FREE which takes a handle.
         * The kernel routes it to the correct GPU's slab pool
         * because handles are namespaced per GPU. */
        struct {
            uint64_t handle;
        } req{};
        req.handle = alloc.handle;

        // VPU_IOC_FREE = _IOW('V', 0x02, ...)
        unsigned long ioc_free = _IOW(VPU_IOC_MAGIC, 0x02, req);
        int ret = ::ioctl(vpu_fd_, ioc_free, &req);
        if (ret < 0)
            return Result<void, std::string>::error(
                "FREE ioctl failed: " + std::string(strerror(errno)));

        // Also free mirrors
        for (auto mh : alloc.mirror_handles) {
            req.handle = mh;
            ::ioctl(vpu_fd_, ioc_free, &req);
        }
    }

    // Update local stats
    if (alloc.gpu_index < static_cast<uint32_t>(gpus_.size())) {
        auto& gpu = gpus_[alloc.gpu_index];
        if (gpu.vram_used >= alloc.size)
            gpu.vram_used -= alloc.size;
        else
            gpu.vram_used = 0;
    }

    return Result<void, std::string>::ok();
}

/* --------------------------------------------------------------------------
 * P2P copy
 * -------------------------------------------------------------------------- */

Result<void, std::string> MultiGpuManager::p2p_copy(
    uint64_t src_handle, uint64_t dst_handle,
    size_t size, size_t offset) {
    std::lock_guard lock(mu_);

    if (vpu_fd_ < 0)
        return Result<void, std::string>::error("VPU device not open");

    k_vpu_p2p_copy_req req{};
    req.src_handle = src_handle;
    req.dst_handle = dst_handle;
    req.size = static_cast<uint64_t>(size);
    req.offset = static_cast<uint64_t>(offset);

    int ret = ::ioctl(vpu_fd_, VPU_IOC_P2P_COPY, &req);
    if (ret < 0)
        return Result<void, std::string>::error(
            "P2P_COPY ioctl failed: " + std::string(strerror(errno)));

    return Result<void, std::string>::ok();
}

/* --------------------------------------------------------------------------
 * Mirror
 * -------------------------------------------------------------------------- */

Result<MgpuAllocation, std::string> MultiGpuManager::mirror(
    uint64_t src_handle, uint32_t gpu_mask) {
    std::lock_guard lock(mu_);

    if (vpu_fd_ < 0)
        return Result<MgpuAllocation, std::string>::error(
            "VPU device not open");

    k_vpu_mirror_req req{};
    req.src_handle = src_handle;
    req.gpu_mask = gpu_mask;

    int ret = ::ioctl(vpu_fd_, VPU_IOC_MIRROR, &req);
    if (ret < 0)
        return Result<MgpuAllocation, std::string>::error(
            "MIRROR ioctl failed: " + std::string(strerror(errno)));

    MgpuAllocation alloc{};
    alloc.handle = src_handle;
    alloc.gpu_index = 0; // source stays on its original GPU
    alloc.gpu_addr = 0;  // unchanged
    alloc.size = 0;      // mirrors match source size

    for (uint32_t i = 0; i < req.count; i++)
        alloc.mirror_handles.push_back(req.mirror_handles[i]);

    return Result<MgpuAllocation, std::string>::ok(std::move(alloc));
}

/* --------------------------------------------------------------------------
 * Per-GPU stats
 * -------------------------------------------------------------------------- */

Result<GpuDeviceInfo, std::string> MultiGpuManager::gpu_stats(
    uint32_t gpu_index) const {
    std::lock_guard lock(mu_);

    if (gpu_index >= static_cast<uint32_t>(gpus_.size()))
        return Result<GpuDeviceInfo, std::string>::error(
            "GPU index " + std::to_string(gpu_index) + " out of range");

    // Try ioctl for fresh data
    if (vpu_fd_ >= 0) {
        k_vpu_gpu_stats_req req{};
        req.gpu_index = gpu_index;

        int ret = ::ioctl(vpu_fd_, VPU_IOC_GPU_STATS, &req);
        if (ret == 0) {
            GpuDeviceInfo info{};
            info.index = req.info.index;
            info.pci_vendor = req.info.pci_vendor;
            info.pci_device = req.info.pci_device;
            info.vendor = vendor_string(req.info.pci_vendor);
            info.name = device_name(req.info.pci_vendor,
                                     req.info.pci_device);
            info.vram_total = static_cast<size_t>(req.info.vram_total);
            info.vram_used = static_cast<size_t>(req.info.vram_used);
            info.p2p_peers = static_cast<int>(req.info.p2p_peers);
            info.pci_slot = std::string(req.info.pci_slot,
                strnlen(req.info.pci_slot, sizeof(req.info.pci_slot)));

            if (req.info.temperature < 0)
                info.temperature = -1.0f;
            else
                info.temperature =
                    static_cast<float>(req.info.temperature) / 1000.0f;

            return Result<GpuDeviceInfo, std::string>::ok(std::move(info));
        }
    }

    // Fallback: try sysfs
    GpuDeviceInfo sysfs_info = read_gpu_sysfs(gpu_index);
    if (!sysfs_info.vendor.empty())
        return Result<GpuDeviceInfo, std::string>::ok(std::move(sysfs_info));

    // Last resort: return cached data
    return Result<GpuDeviceInfo, std::string>::ok(gpus_[gpu_index]);
}

/* --------------------------------------------------------------------------
 * P2P capability check
 * -------------------------------------------------------------------------- */

bool MultiGpuManager::has_p2p(uint32_t gpu_a, uint32_t gpu_b) const {
    std::lock_guard lock(mu_);

    if (gpu_a >= static_cast<uint32_t>(gpus_.size()) ||
        gpu_b >= static_cast<uint32_t>(gpus_.size()))
        return false;

    if (gpu_a == gpu_b)
        return true;

    // Check the bitmask from either GPU
    return (gpus_[gpu_a].p2p_peers & (1 << gpu_b)) != 0;
}

} // namespace straylight::hw
