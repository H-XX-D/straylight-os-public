// bin/pmem/log.cpp
#include "log.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace straylight::pmem {

WriteAheadLog::~WriteAheadLog() {
    if (mapped_ && mapped_ != MAP_FAILED) {
        ::msync(mapped_, capacity_, MS_SYNC);
        ::munmap(mapped_, capacity_);
    }
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

Result<void, std::string> WriteAheadLog::init(const std::string& path, size_t capacity) {
    if (capacity < sizeof(WalHeader) + sizeof(EntryHeader) + 64) {
        return Result<void, std::string>::error("WAL capacity too small");
    }

    path_ = path;
    capacity_ = capacity;

    // Open or create the WAL file.
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd_ < 0) {
        return Result<void, std::string>::error(
            "Cannot open WAL " + path + ": " + std::strerror(errno));
    }

    // Ensure file is the right size.
    struct stat st{};
    if (::fstat(fd_, &st) != 0) {
        return Result<void, std::string>::error("fstat failed: " + std::string(std::strerror(errno)));
    }

    bool is_new = (st.st_size < static_cast<off_t>(sizeof(WalHeader)));
    if (static_cast<size_t>(st.st_size) < capacity) {
        if (::ftruncate(fd_, static_cast<off_t>(capacity)) != 0) {
            return Result<void, std::string>::error("ftruncate failed: " + std::string(std::strerror(errno)));
        }
    }

    // Memory-map.
    mapped_ = ::mmap(nullptr, capacity, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (mapped_ == MAP_FAILED) {
        mapped_ = nullptr;
        return Result<void, std::string>::error("mmap failed: " + std::string(std::strerror(errno)));
    }

    if (is_new || header()->magic != WAL_MAGIC) {
        // Initialize fresh WAL.
        std::memset(mapped_, 0, capacity);
        header()->magic = WAL_MAGIC;
        header()->version = WAL_VERSION;
        header()->next_lsn = 1;
        header()->write_offset = sizeof(WalHeader);
        header()->capacity = capacity;
        flush_range(mapped_, sizeof(WalHeader));
    }

    // Restore state from header.
    next_lsn_ = header()->next_lsn;
    write_offset_ = header()->write_offset;

    return Result<void, std::string>::ok();
}

Result<uint64_t, std::string> WriteAheadLog::append(const void* data, size_t len) {
    if (!mapped_) {
        return Result<uint64_t, std::string>::error("WAL not initialized");
    }
    if (len == 0) {
        return Result<uint64_t, std::string>::error("Cannot append zero-length entry");
    }

    size_t entry_size = sizeof(EntryHeader) + len;
    // Align to 8 bytes for the next entry.
    size_t aligned_size = (entry_size + 7) & ~static_cast<size_t>(7);

    if (write_offset_ + aligned_size > capacity_) {
        return Result<uint64_t, std::string>::error(
            "WAL full: need " + std::to_string(aligned_size) +
            " bytes, have " + std::to_string(capacity_ - write_offset_));
    }

    uint64_t lsn = next_lsn_++;

    // Write entry header.
    auto* entry = reinterpret_cast<EntryHeader*>(
        reinterpret_cast<uint8_t*>(mapped_) + write_offset_);
    entry->lsn = lsn;
    entry->data_len = static_cast<uint32_t>(len);
    entry->flags = 0; // Not committed yet.
    entry->checksum = crc32(data, len);
    entry->padding = 0;

    // Write data.
    std::memcpy(reinterpret_cast<uint8_t*>(entry) + sizeof(EntryHeader), data, len);

    // Flush entry to persistence.
    flush_range(entry, aligned_size);

    write_offset_ += aligned_size;

    // Update header.
    header()->next_lsn = next_lsn_;
    header()->write_offset = write_offset_;
    flush_range(header(), sizeof(WalHeader));

    return Result<uint64_t, std::string>::ok(lsn);
}

Result<void, std::string> WriteAheadLog::commit(uint64_t lsn) {
    if (!mapped_) {
        return Result<void, std::string>::error("WAL not initialized");
    }

    // Scan entries to find the one with matching LSN.
    size_t offset = sizeof(WalHeader);
    while (offset + sizeof(EntryHeader) <= write_offset_) {
        auto* entry = reinterpret_cast<EntryHeader*>(
            reinterpret_cast<uint8_t*>(mapped_) + offset);

        if (entry->lsn == lsn) {
            entry->flags |= FLAG_COMMITTED;
            flush_range(&entry->flags, sizeof(entry->flags));
            return Result<void, std::string>::ok();
        }

        size_t entry_size = sizeof(EntryHeader) + entry->data_len;
        size_t aligned = (entry_size + 7) & ~static_cast<size_t>(7);
        offset += aligned;
    }

    return Result<void, std::string>::error(
        "LSN " + std::to_string(lsn) + " not found in WAL");
}

Result<std::vector<LogEntry>, std::string> WriteAheadLog::recover() {
    if (!mapped_) {
        return Result<std::vector<LogEntry>, std::string>::error("WAL not initialized");
    }

    std::vector<LogEntry> entries;
    size_t offset = sizeof(WalHeader);

    while (offset + sizeof(EntryHeader) <= write_offset_) {
        auto* eh = reinterpret_cast<const EntryHeader*>(
            reinterpret_cast<const uint8_t*>(mapped_) + offset);

        size_t entry_size = sizeof(EntryHeader) + eh->data_len;
        size_t aligned = (entry_size + 7) & ~static_cast<size_t>(7);

        if (offset + entry_size > capacity_) {
            break; // Truncated entry — crash during write.
        }

        // Verify checksum.
        const uint8_t* data_ptr = reinterpret_cast<const uint8_t*>(eh) + sizeof(EntryHeader);
        uint32_t computed_crc = crc32(data_ptr, eh->data_len);

        if (computed_crc == eh->checksum) {
            LogEntry le;
            le.lsn = eh->lsn;
            le.committed = (eh->flags & FLAG_COMMITTED) != 0;
            le.data.assign(data_ptr, data_ptr + eh->data_len);
            entries.push_back(std::move(le));
        }
        // If checksum fails, skip this entry (corrupted).

        offset += aligned;
    }

    return Result<std::vector<LogEntry>, std::string>::ok(std::move(entries));
}

void WriteAheadLog::flush_range(void* addr, size_t len) {
    // Align to page boundary for msync.
    uintptr_t page_mask = ~(static_cast<uintptr_t>(4096) - 1);
    void* aligned = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(addr) & page_mask);
    size_t aligned_len = len + (reinterpret_cast<uintptr_t>(addr) -
                                 reinterpret_cast<uintptr_t>(aligned));
    // Round up to page size.
    aligned_len = (aligned_len + 4095) & ~static_cast<size_t>(4095);
    ::msync(aligned, aligned_len, MS_SYNC);
}

uint32_t WriteAheadLog::crc32(const void* data, size_t len) {
    // CRC-32/ISO-HDLC (standard Ethernet CRC).
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int j = 0; j < 8; ++j) {
                c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
        return t;
    }();

    auto* p = reinterpret_cast<const uint8_t*>(data);
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

} // namespace straylight::pmem
