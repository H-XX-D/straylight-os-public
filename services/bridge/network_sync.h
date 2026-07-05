/**
 * StrayLight Bridge Network Sync — Page transfer between nodes.
 *
 * TLS connection to remote bridge daemon with delta compression
 * (only send changed bytes within dirty pages). Provides sequential
 * consistency within a bridge and Whisper encryption for sensitive regions.
 */
#pragma once

#include "page_tracker.h"
#include "straylight/result.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace straylight::bridge {

/// Sync mode for a bridge.
enum class SyncMode {
    Immediate,  // Every write syncs — low latency, high bandwidth
    Batched,    // Sync every N ms — balanced
    Manual      // App calls sync explicitly
};

inline const char* sync_mode_to_string(SyncMode m) {
    switch (m) {
        case SyncMode::Immediate: return "immediate";
        case SyncMode::Batched:   return "batched";
        case SyncMode::Manual:    return "manual";
    }
    return "unknown";
}

inline SyncMode sync_mode_from_string(const std::string& s) {
    if (s == "immediate") return SyncMode::Immediate;
    if (s == "manual") return SyncMode::Manual;
    return SyncMode::Batched;
}

/// Page sync wire protocol header.
struct PageSyncHeader {
    uint32_t magic;         // 0x534C4250 "SLBP"
    uint32_t bridge_id;
    uint64_t sequence;      // Monotonic for ordering guarantees
    uint32_t num_pages;
    uint32_t total_size;    // Total payload size after header
    uint32_t checksum;
    uint8_t  flags;         // bit 0: delta, bit 1: encrypted
    uint8_t  reserved[3];

    static constexpr uint32_t kMagic = 0x534C4250;
};

/// A single page update in the sync batch.
struct PageUpdate {
    uint64_t offset;
    uint32_t size;
    std::vector<uint8_t> data;      // Full page data or delta
    bool is_delta;                   // True if data is a delta, not full page
};

/// Statistics for a network sync connection.
struct SyncStats {
    uint64_t total_syncs;
    uint64_t total_pages_sent;
    uint64_t total_bytes_sent;
    uint64_t total_bytes_saved_delta; // Bytes saved by delta compression
    double avg_sync_latency_ms;
    double throughput_mbps;
    std::chrono::steady_clock::time_point last_sync_time;
};

class NetworkSync {
public:
    NetworkSync();
    ~NetworkSync();

    /// Connect to a remote bridge daemon.
    VoidResult<std::string> connect(const std::string& host, uint16_t port);

    /// Accept incoming connection.
    VoidResult<std::string> listen(uint16_t port);

    /// Send dirty pages to the remote side.
    VoidResult<std::string> sync_pages(uint32_t bridge_id,
                                        const std::vector<DirtyPage>& pages,
                                        const void* base_addr);

    /// Receive pages from remote and apply to local memory.
    Result<uint32_t, std::string> receive_and_apply(void* base_addr, size_t region_size);

    /// Enable Whisper encryption for sensitive regions.
    void enable_encryption(bool enable) { encryption_enabled_ = enable; }

    /// Set sync mode.
    void set_sync_mode(SyncMode mode) { sync_mode_ = mode; }
    [[nodiscard]] SyncMode sync_mode() const { return sync_mode_; }

    /// Set batch interval for batched mode.
    void set_batch_interval_ms(int ms) { batch_interval_ms_ = ms; }

    /// Get statistics.
    SyncStats get_stats() const;

    /// Close connection.
    void close();

    /// Check if connected.
    [[nodiscard]] bool is_connected() const { return socket_fd_ >= 0; }

    /// Compute delta between old and new page content.
    static std::vector<uint8_t> compute_page_delta(const uint8_t* old_data,
                                                     const uint8_t* new_data,
                                                     size_t page_size);

    /// Apply delta to reconstruct page content.
    static void apply_page_delta(uint8_t* page_data,
                                  const std::vector<uint8_t>& delta,
                                  size_t page_size);

private:
    int socket_fd_ = -1;
    int listen_fd_ = -1;
    SyncMode sync_mode_ = SyncMode::Batched;
    int batch_interval_ms_ = 10;
    bool encryption_enabled_ = false;
    uint64_t sync_sequence_ = 0;

    mutable std::mutex stats_mutex_;
    SyncStats stats_;

    // Previous page contents for delta computation.
    std::vector<std::vector<uint8_t>> page_shadows_;

    /// Send exactly n bytes.
    VoidResult<std::string> send_all(const void* buf, size_t len);

    /// Receive exactly n bytes.
    VoidResult<std::string> recv_all(void* buf, size_t len);

    /// Simple XOR-based encryption (Whisper placeholder).
    static void whisper_encrypt(uint8_t* data, size_t len, uint64_t key);
    static void whisper_decrypt(uint8_t* data, size_t len, uint64_t key);

    /// CRC32 checksum.
    static uint32_t crc32(const void* data, size_t len);
};

} // namespace straylight::bridge
