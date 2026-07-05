// bin/pmem/log.h
// Write-ahead log on persistent memory for crash safety
#pragma once

#include <straylight/result.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace straylight::pmem {

/// A single log entry recovered from the WAL.
struct LogEntry {
    uint64_t lsn;           // Log sequence number.
    bool committed;         // Whether this entry was committed before crash.
    std::vector<uint8_t> data;
};

class WriteAheadLog {
public:
    ~WriteAheadLog();

    /// Initialize (or open existing) WAL at the given path with max capacity.
    Result<void, std::string> init(const std::string& path, size_t capacity);

    /// Append data to the log. Returns the LSN of the new entry.
    Result<uint64_t, std::string> append(const void* data, size_t len);

    /// Mark an entry as committed (durable).
    Result<void, std::string> commit(uint64_t lsn);

    /// Recover all entries from the WAL (for crash recovery).
    Result<std::vector<LogEntry>, std::string> recover();

    [[nodiscard]] uint64_t current_lsn() const { return next_lsn_; }

private:
    // On-disk WAL header (at offset 0 of the mapped file).
    struct WalHeader {
        uint64_t magic;         // Magic number for validation.
        uint64_t version;       // Format version.
        uint64_t next_lsn;      // Next LSN to assign.
        uint64_t write_offset;  // Current write position after header.
        uint64_t capacity;      // Total file capacity.
    };

    // On-disk entry header (precedes each log record).
    struct EntryHeader {
        uint64_t lsn;
        uint32_t data_len;
        uint32_t flags;         // Bit 0: committed.
        uint32_t checksum;      // CRC32 of data.
        uint32_t padding;
    };

    static constexpr uint64_t WAL_MAGIC = 0x53544C57414C0001ULL; // "STLWAL\x00\x01"
    static constexpr uint64_t WAL_VERSION = 1;
    static constexpr uint32_t FLAG_COMMITTED = 0x01;

    std::string path_;
    int fd_ = -1;
    void* mapped_ = nullptr;
    size_t capacity_ = 0;
    uint64_t next_lsn_ = 1;
    size_t write_offset_ = 0;

    WalHeader* header() { return reinterpret_cast<WalHeader*>(mapped_); }
    const WalHeader* header() const { return reinterpret_cast<const WalHeader*>(mapped_); }

    void flush_range(void* addr, size_t len);
    static uint32_t crc32(const void* data, size_t len);
};

} // namespace straylight::pmem
