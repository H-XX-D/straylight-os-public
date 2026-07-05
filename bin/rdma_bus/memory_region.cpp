// bin/rdma_bus/memory_region.cpp
#include "memory_region.h"
#include "verbs.h"

#include <infiniband/verbs.h>

#include <cerrno>
#include <cstring>

namespace straylight::rdma {

MemoryRegionManager::MemoryRegionManager(VerbsContext& verbs)
    : verbs_(verbs) {}

MemoryRegionManager::~MemoryRegionManager() {
    // Deregister all remaining regions
    for (auto& [handle, managed] : regions_) {
        if (managed.mr) {
            ibv_dereg_mr(managed.mr);
            managed.mr = nullptr;
        }
    }
}

Result<RegionHandle, std::string> MemoryRegionManager::register_region(void* ptr, size_t size) {
    if (!ptr || size == 0) {
        return Result<RegionHandle, std::string>::error("Invalid buffer for MR registration");
    }
    if (!verbs_.pd()) {
        return Result<RegionHandle, std::string>::error(
            "Protection domain not created; call verbs.create_pd() first");
    }

    int access = IBV_ACCESS_LOCAL_WRITE |
                 IBV_ACCESS_REMOTE_WRITE |
                 IBV_ACCESS_REMOTE_READ;

    struct ibv_mr* mr = ibv_reg_mr(verbs_.pd(), ptr, size, access);
    if (!mr) {
        return Result<RegionHandle, std::string>::error(
            std::string("ibv_reg_mr failed: ") + std::strerror(errno));
    }

    RegionHandle handle = next_handle_++;
    ManagedRegion managed;
    managed.mr        = mr;
    managed.info.addr = ptr;
    managed.info.size = size;
    managed.info.lkey = mr->lkey;
    managed.info.rkey = mr->rkey;

    regions_[handle] = managed;

    return Result<RegionHandle, std::string>::ok(handle);
}

Result<void, std::string> MemoryRegionManager::deregister(RegionHandle handle) {
    auto it = regions_.find(handle);
    if (it == regions_.end()) {
        return Result<void, std::string>::error(
            "Region handle " + std::to_string(handle) + " not found");
    }

    if (it->second.mr) {
        int ret = ibv_dereg_mr(it->second.mr);
        if (ret) {
            return Result<void, std::string>::error(
                std::string("ibv_dereg_mr failed: ") + std::strerror(errno));
        }
    }

    regions_.erase(it);
    return Result<void, std::string>::ok();
}

const RegionInfo* MemoryRegionManager::get_info(RegionHandle handle) const {
    auto it = regions_.find(handle);
    if (it == regions_.end()) {
        return nullptr;
    }
    return &it->second.info;
}

size_t MemoryRegionManager::count() const noexcept {
    return regions_.size();
}

} // namespace straylight::rdma
