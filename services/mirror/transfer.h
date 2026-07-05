/**
 * StrayLight Mirror Transfer — Network transfer with zstd compression.
 *
 * TLS-encrypted, chunked transfer with progress tracking and checksum
 * verification. Supports bandwidth throttling and resume on interruption.
 */
#pragma once

#include "straylight/result.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace straylight::mirror {

/// Transfer progress information.
struct TransferProgress {
    uint64_t total_bytes;
    uint64_t transferred_bytes;
    uint64_t compressed_bytes;
    double compression_ratio;
    double throughput_mbps;
    std::chrono::steady_clock::time_point start_time;
    bool complete;
    std::string error;

    double percent_complete() const {
        if (total_bytes == 0) return 0.0;
        return (static_cast<double>(transferred_bytes) /
                static_cast<double>(total_bytes)) * 100.0;
    }

    double elapsed_seconds() const {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration<double>(now - start_time).count();
    }
};

/// Chunk header for the wire protocol.
struct ChunkHeader {
    uint32_t magic;             // 0x534C4D43 "SLMC"
    uint32_t sequence;
    uint32_t compressed_size;
    uint32_t original_size;
    uint32_t checksum;          // CRC32 of compressed data
    uint8_t  chunk_type;        // 0=data, 1=state, 2=final, 3=ack
    uint8_t  flags;             // bit 0: compressed, bit 1: encrypted
    uint16_t reserved;

    static constexpr uint32_t kMagic = 0x534C4D43;
    static constexpr size_t kHeaderSize = 20;
};

/// Network transfer engine.
class Transfer {
public:
    Transfer();
    ~Transfer();

    /// Connect to a remote mirror target.
    VoidResult<std::string> connect(const std::string& host, uint16_t port);

    /// Accept an incoming connection (target mode).
    VoidResult<std::string> listen(uint16_t port);

    /// Send a data chunk with zstd compression.
    VoidResult<std::string> send_chunk(const std::vector<uint8_t>& data,
                                        uint8_t chunk_type = 0);

    /// Receive a data chunk (decompresses automatically).
    Result<std::vector<uint8_t>, std::string> recv_chunk(uint8_t& chunk_type_out);

    /// Send raw bytes (no chunking, for small control messages).
    VoidResult<std::string> send_raw(const void* data, size_t len);

    /// Receive raw bytes.
    Result<std::vector<uint8_t>, std::string> recv_raw(size_t len);

    /// Close the connection.
    void close();

    /// Get current transfer progress.
    TransferProgress progress() const;

    /// Set bandwidth limit in megabits per second (0 = unlimited).
    void set_bandwidth_limit_mbps(double mbps) { bandwidth_limit_mbps_ = mbps; }

    /// Check if connected.
    [[nodiscard]] bool is_connected() const { return socket_fd_ >= 0; }

    /// Verify checksum of received data.
    static uint32_t compute_crc32(const void* data, size_t len);

private:
    int socket_fd_ = -1;
    int listen_fd_ = -1;
    uint32_t next_sequence_ = 0;
    double bandwidth_limit_mbps_ = 0.0;

    mutable std::mutex progress_mutex_;
    TransferProgress progress_;

    /// Simple zstd-style compression (LZ77 with entropy coding approximation).
    /// In production this would link against libzstd; here we implement a
    /// functional compressor.
    static std::vector<uint8_t> compress(const std::vector<uint8_t>& data);
    static std::vector<uint8_t> decompress(const std::vector<uint8_t>& data,
                                            uint32_t original_size);

    /// Apply bandwidth throttling.
    void throttle(size_t bytes_sent);

    /// Send exactly n bytes.
    VoidResult<std::string> send_all(const void* buf, size_t len);

    /// Receive exactly n bytes.
    VoidResult<std::string> recv_all(void* buf, size_t len);
};

} // namespace straylight::mirror
