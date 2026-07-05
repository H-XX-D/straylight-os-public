// bin/pmem/dax.cpp
#include "dax.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace straylight::pmem {

DaxManager::~DaxManager() {
    for (auto& [addr, region] : mappings_) {
        ::munmap(addr, region.size);
    }
    mappings_.clear();
}

Result<void*, std::string> DaxManager::map_region(const std::string& dax_path, size_t size) {
    if (size == 0) {
        return Result<void*, std::string>::error("Cannot map zero-size region");
    }

    int flags = O_RDWR;

    // Try to open existing file/device.
    int fd = ::open(dax_path.c_str(), flags);
    if (fd < 0) {
        // If not a device, try creating a regular file for simulation.
        if (dax_path.starts_with("/dev/")) {
            return Result<void*, std::string>::error(
                "Cannot open DAX device " + dax_path + ": " + std::strerror(errno));
        }
        fd = ::open(dax_path.c_str(), O_RDWR | O_CREAT, 0644);
        if (fd < 0) {
            return Result<void*, std::string>::error(
                "Cannot create " + dax_path + ": " + std::strerror(errno));
        }
    }

    // Ensure file is large enough.
    struct stat st{};
    if (::fstat(fd, &st) == 0 && static_cast<size_t>(st.st_size) < size) {
        if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
            int err = errno;
            ::close(fd);
            return Result<void*, std::string>::error(
                "ftruncate failed: " + std::string(std::strerror(err)));
        }
    }

    // mmap with MAP_SHARED for persistence semantics.
    int mmap_flags = MAP_SHARED;
    void* addr = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, mmap_flags, fd, 0);
    ::close(fd);

    if (addr == MAP_FAILED) {
        return Result<void*, std::string>::error(
            "mmap failed: " + std::string(std::strerror(errno)));
    }

    bool pmem = is_pmem(dax_path);
    mappings_[addr] = {dax_path, size, pmem};
    return Result<void*, std::string>::ok(addr);
}

Result<void, std::string> DaxManager::unmap(void* addr, size_t size) {
    auto it = mappings_.find(addr);
    if (it == mappings_.end()) {
        return Result<void, std::string>::error("Address not mapped by this DaxManager");
    }
    if (it->second.size != size) {
        return Result<void, std::string>::error(
            "Size mismatch: mapped " + std::to_string(it->second.size) +
            " but unmap requested " + std::to_string(size));
    }

    if (::munmap(addr, size) != 0) {
        return Result<void, std::string>::error(
            "munmap failed: " + std::string(std::strerror(errno)));
    }

    mappings_.erase(it);
    return Result<void, std::string>::ok();
}

Result<void, std::string> DaxManager::flush(void* addr, size_t size) {
    // Check if this address falls within any of our mappings.
    bool found = false;
    bool is_pmem_region = false;
    for (const auto& [base, region] : mappings_) {
        auto base_u = reinterpret_cast<uintptr_t>(base);
        auto addr_u = reinterpret_cast<uintptr_t>(addr);
        if (addr_u >= base_u && addr_u + size <= base_u + region.size) {
            found = true;
            is_pmem_region = region.is_pmem;
            break;
        }
    }

    if (!found) {
        return Result<void, std::string>::error("Address not within any mapped region");
    }

    if (is_pmem_region) {
        // On real pmem hardware, we would use CLWB/CLFLUSHOPT + SFENCE.
        // Inline asm for x86_64 persistent memory flush:
        //   for each cache line in [addr, addr+size):
        //     _mm_clwb(ptr) or clflushopt
        //   _mm_sfence()
        // Since this is a library that may run on non-pmem hardware too,
        // we provide the optimized path but fall through to msync.
#if defined(__x86_64__) && defined(__linux__)
        const size_t CACHE_LINE = 64;
        auto* p = reinterpret_cast<volatile char*>(addr);
        for (size_t offset = 0; offset < size; offset += CACHE_LINE) {
            // clwb instruction: 0x66, 0x0f, 0xae, 0x30 + reg
            asm volatile(".byte 0x66; xsaveopt %0" : "+m"(*(p + offset)));
        }
        asm volatile("sfence" ::: "memory");
        return Result<void, std::string>::ok();
#endif
    }

    // Fallback: msync for file-backed mappings.
    // Align to page boundary.
    uintptr_t page_mask = ~(static_cast<uintptr_t>(4096) - 1);
    void* aligned = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(addr) & page_mask);
    size_t aligned_size = size + (reinterpret_cast<uintptr_t>(addr) -
                                   reinterpret_cast<uintptr_t>(aligned));

    if (::msync(aligned, aligned_size, MS_SYNC) != 0) {
        return Result<void, std::string>::error(
            "msync failed: " + std::string(std::strerror(errno)));
    }

    return Result<void, std::string>::ok();
}

bool DaxManager::is_pmem(const std::string& path) const {
    // Real detection: check /sys/bus/nd/devices/ or use libndctl.
    // For now, we detect DAX character devices.
    if (path.starts_with("/dev/dax")) {
        return true;
    }
    // Check for devdax via sysfs.
    struct stat st{};
    if (::stat(path.c_str(), &st) == 0 && S_ISCHR(st.st_mode)) {
        // Character device in /dev/dax* namespace is likely pmem.
        return path.find("dax") != std::string::npos;
    }
    return false;
}

} // namespace straylight::pmem
