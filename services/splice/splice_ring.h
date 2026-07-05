// services/splice/splice_ring.h
// Header-only lock-free SPSC ring buffer for zero-copy pipeline stitching.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <algorithm>

namespace straylight {

/// Flags for the splice ring control block.
enum class SpliceRingFlags : uint32_t {
    None        = 0,
    ProducerEOF = 1 << 0,   // Producer has closed its end
    ConsumerEOF = 1 << 1,   // Consumer has closed its end
    Error       = 1 << 2,   // An error was signaled
};

inline SpliceRingFlags operator|(SpliceRingFlags a, SpliceRingFlags b) {
    return static_cast<SpliceRingFlags>(
        static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline SpliceRingFlags operator&(SpliceRingFlags a, SpliceRingFlags b) {
    return static_cast<SpliceRingFlags>(
        static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

inline bool has_flag(SpliceRingFlags flags, SpliceRingFlags test) {
    return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(test)) != 0;
}

/// Lock-free single-producer single-consumer ring buffer.
///
/// Layout in shared memory:
///   [SpliceRing header]  (this struct, cache-line aligned)
///   [data buffer]        (capacity_ bytes, immediately after header)
///
/// The producer calls push(), the consumer calls pop().
/// Memory ordering: write_pos_ uses release on store, acquire on load.
///                  read_pos_ uses release on store, acquire on load.
/// This ensures the data written by the producer is visible to the consumer.
struct alignas(64) SpliceRing {
    std::atomic<uint64_t> write_pos_{0};
    char pad_w_[64 - sizeof(std::atomic<uint64_t>)];

    std::atomic<uint64_t> read_pos_{0};
    char pad_r_[64 - sizeof(std::atomic<uint64_t>)];

    uint64_t capacity_{0};
    std::atomic<uint32_t> flags_{0};

    /// Initialize the ring with the given capacity.
    /// Must be called once before use (by whoever sets up the shared region).
    void init(uint64_t cap) {
        write_pos_.store(0, std::memory_order_relaxed);
        read_pos_.store(0, std::memory_order_relaxed);
        capacity_ = cap;
        flags_.store(0, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
    }

    /// Pointer to the data region (immediately after the header).
    uint8_t* data() {
        return reinterpret_cast<uint8_t*>(this) + sizeof(SpliceRing);
    }

    const uint8_t* data() const {
        return reinterpret_cast<const uint8_t*>(this) + sizeof(SpliceRing);
    }

    /// Number of bytes available to read.
    [[nodiscard]] uint64_t available() const {
        uint64_t w = write_pos_.load(std::memory_order_acquire);
        uint64_t r = read_pos_.load(std::memory_order_relaxed);
        return w - r;
    }

    /// Number of bytes of free space for writing.
    [[nodiscard]] uint64_t free_space() const {
        uint64_t w = write_pos_.load(std::memory_order_relaxed);
        uint64_t r = read_pos_.load(std::memory_order_acquire);
        return capacity_ - (w - r);
    }

    /// Total capacity in bytes.
    [[nodiscard]] uint64_t capacity() const { return capacity_; }

    /// True if no data is available to read.
    [[nodiscard]] bool is_empty() const { return available() == 0; }

    /// True if no space is available to write.
    [[nodiscard]] bool is_full() const { return free_space() == 0; }

    /// Get current flags.
    [[nodiscard]] SpliceRingFlags get_flags() const {
        return static_cast<SpliceRingFlags>(flags_.load(std::memory_order_acquire));
    }

    /// Set a flag atomically.
    void set_flag(SpliceRingFlags flag) {
        flags_.fetch_or(static_cast<uint32_t>(flag), std::memory_order_release);
    }

    /// Push data into the ring buffer (producer side).
    /// Returns the number of bytes actually written (may be less than len if full).
    uint64_t push(const void* src, uint64_t len) {
        uint64_t w = write_pos_.load(std::memory_order_relaxed);
        uint64_t r = read_pos_.load(std::memory_order_acquire);
        uint64_t space = capacity_ - (w - r);
        uint64_t to_write = std::min(len, space);

        if (to_write == 0) return 0;

        const auto* src_bytes = static_cast<const uint8_t*>(src);
        uint8_t* buf = data();

        // Write may wrap around the ring
        uint64_t offset = w % capacity_;
        uint64_t first_chunk = std::min(to_write, capacity_ - offset);
        std::memcpy(buf + offset, src_bytes, first_chunk);

        if (to_write > first_chunk) {
            std::memcpy(buf, src_bytes + first_chunk, to_write - first_chunk);
        }

        // Release ensures the data is visible before we advance write_pos_
        write_pos_.store(w + to_write, std::memory_order_release);
        return to_write;
    }

    /// Pop data from the ring buffer (consumer side).
    /// Returns the number of bytes actually read (may be less than max_len if empty).
    uint64_t pop(void* dst, uint64_t max_len) {
        uint64_t r = read_pos_.load(std::memory_order_relaxed);
        uint64_t w = write_pos_.load(std::memory_order_acquire);
        uint64_t avail = w - r;
        uint64_t to_read = std::min(max_len, avail);

        if (to_read == 0) return 0;

        auto* dst_bytes = static_cast<uint8_t*>(dst);
        const uint8_t* buf = data();

        // Read may wrap around the ring
        uint64_t offset = r % capacity_;
        uint64_t first_chunk = std::min(to_read, capacity_ - offset);
        std::memcpy(dst_bytes, buf + offset, first_chunk);

        if (to_read > first_chunk) {
            std::memcpy(dst_bytes + first_chunk, buf, to_read - first_chunk);
        }

        // Release ensures we don't advance read_pos_ before we've copied the data
        read_pos_.store(r + to_read, std::memory_order_release);
        return to_read;
    }

    /// Peek at data without consuming it. Returns bytes copied.
    uint64_t peek(void* dst, uint64_t max_len) const {
        uint64_t r = read_pos_.load(std::memory_order_relaxed);
        uint64_t w = write_pos_.load(std::memory_order_acquire);
        uint64_t avail = w - r;
        uint64_t to_read = std::min(max_len, avail);

        if (to_read == 0) return 0;

        auto* dst_bytes = static_cast<uint8_t*>(dst);
        const uint8_t* buf = data();

        uint64_t offset = r % capacity_;
        uint64_t first_chunk = std::min(to_read, capacity_ - offset);
        std::memcpy(dst_bytes, buf + offset, first_chunk);

        if (to_read > first_chunk) {
            std::memcpy(dst_bytes + first_chunk, buf, to_read - first_chunk);
        }

        return to_read;
    }
};

/// Compute the total shared memory size needed for a ring of the given data capacity.
inline uint64_t splice_ring_total_size(uint64_t data_capacity) {
    return sizeof(SpliceRing) + data_capacity;
}

} // namespace straylight
