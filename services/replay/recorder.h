// services/replay/recorder.h
// Event recording engine for the StrayLight flight recorder.
// Captures system events into a lock-free mmap'd ring buffer.
#pragma once

#include <straylight/result.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace straylight {

/// Classification of system events captured by the recorder.
enum class EventType : uint8_t {
    Syscall         = 0,
    ProcessStart    = 1,
    ProcessExit     = 2,
    Signal          = 3,
    FileOpen        = 4,
    FileWrite       = 5,
    NetworkConnect  = 6,
    NetworkListen   = 7,
    GpuAlloc        = 8,
    GpuFree         = 9,
    GpuSubmit       = 10,
    ServiceStart    = 11,
    ServiceStop     = 12,
    ServiceFail     = 13,
    DmesgEntry      = 14,
    OomKill         = 15,
    Panic           = 16,
    UserLogin       = 17,
    UserLogout      = 18,
};

/// Human-readable name for an event type.
inline const char* event_type_name(EventType t) {
    switch (t) {
        case EventType::Syscall:        return "syscall";
        case EventType::ProcessStart:   return "process_start";
        case EventType::ProcessExit:    return "process_exit";
        case EventType::Signal:         return "signal";
        case EventType::FileOpen:       return "file_open";
        case EventType::FileWrite:      return "file_write";
        case EventType::NetworkConnect: return "network_connect";
        case EventType::NetworkListen:  return "network_listen";
        case EventType::GpuAlloc:       return "gpu_alloc";
        case EventType::GpuFree:        return "gpu_free";
        case EventType::GpuSubmit:      return "gpu_submit";
        case EventType::ServiceStart:   return "service_start";
        case EventType::ServiceStop:    return "service_stop";
        case EventType::ServiceFail:    return "service_fail";
        case EventType::DmesgEntry:     return "dmesg";
        case EventType::OomKill:        return "oom_kill";
        case EventType::Panic:          return "panic";
        case EventType::UserLogin:      return "user_login";
        case EventType::UserLogout:     return "user_logout";
    }
    return "unknown";
}

/// A single captured system event.
struct SystemEvent {
    uint64_t    timestamp_ns;   ///< clock_gettime(CLOCK_MONOTONIC) nanoseconds
    EventType   type;
    uint32_t    pid;
    uint32_t    uid;
    std::string process_name;
    std::string detail;         ///< Event-specific JSON payload
};

/// On-disk header for the mmap'd ring buffer file.
struct RingBufferHeader {
    uint64_t magic;             ///< 0x5354524159524550 ("STRAYREP")
    uint64_t version;
    uint64_t buffer_size;       ///< Total file size in bytes
    uint64_t data_offset;       ///< Offset to first event slot
    std::atomic<uint64_t> write_offset;   ///< Current write position (bytes from data_offset)
    std::atomic<uint64_t> event_count;    ///< Total events written (wraps with buffer)
    uint64_t oldest_timestamp_ns;
    uint64_t reserved[8];
};

/// Serialized event record in the ring buffer.
/// Variable length: fixed header + process_name + detail.
struct EventRecord {
    uint32_t record_size;       ///< Total size of this record including header
    uint64_t timestamp_ns;
    EventType type;
    uint32_t pid;
    uint32_t uid;
    uint16_t name_len;
    uint16_t detail_len;
    // Followed by: char process_name[name_len], char detail[detail_len]
};

static constexpr uint64_t RING_BUFFER_MAGIC = 0x5354524159524550ULL;
static constexpr uint64_t RING_BUFFER_VERSION = 1;
static constexpr size_t   DEFAULT_BUFFER_SIZE_MB = 256;
static constexpr size_t   MAX_EVENT_NAME_LEN = 256;
static constexpr size_t   MAX_EVENT_DETAIL_LEN = 4096;

/// Continuously records system events into a lock-free mmap'd ring buffer.
/// Single-writer architecture: only one EventRecorder should write to a buffer.
class EventRecorder {
public:
    EventRecorder();
    ~EventRecorder();

    EventRecorder(const EventRecorder&) = delete;
    EventRecorder& operator=(const EventRecorder&) = delete;

    /// Start the recorder. Creates or opens the ring buffer file and
    /// launches background source threads.
    Result<void, std::string> start(size_t buffer_size_mb = DEFAULT_BUFFER_SIZE_MB);

    /// Stop all source threads and unmap the buffer.
    void stop();

    /// Record a single event into the ring buffer.
    void record(SystemEvent event);

    /// Check if the recorder is currently running.
    [[nodiscard]] bool is_running() const { return running_.load(); }

    /// Get the total number of events recorded since start.
    [[nodiscard]] uint64_t event_count() const;

    /// Get a snapshot of all events currently in the buffer.
    [[nodiscard]] std::vector<SystemEvent> snapshot() const;

    /// Get events matching a filter predicate.
    [[nodiscard]] std::vector<SystemEvent> query(
        std::function<bool(const SystemEvent&)> predicate) const;

    /// Get events within a time range (monotonic nanoseconds).
    [[nodiscard]] std::vector<SystemEvent> events_in_range(
        uint64_t start_ns, uint64_t end_ns) const;

    /// Get the path to the ring buffer file.
    [[nodiscard]] const std::string& buffer_path() const { return buffer_path_; }

private:
    /// Initialize or open the mmap'd ring buffer.
    Result<void, std::string> init_buffer(size_t total_size);

    /// Write a serialized event record into the ring buffer.
    void write_record(const SystemEvent& event);

    /// Read all valid records from the ring buffer.
    std::vector<SystemEvent> read_all_records() const;

    /// Background source threads
    void source_kmsg();          ///< Read /dev/kmsg for kernel messages
    void source_proc_events();   ///< netlink PROC_EVENT for fork/exec/exit
    void source_network_poll();  ///< Periodic /proc/net/tcp,udp scan
    void source_gpu_poll();      ///< Poll /sys/kernel/straylight-vpu/
    void source_dbus_signals();  ///< systemd unit state changes via D-Bus

    std::string buffer_path_ = "/var/lib/straylight/replay/buffer.dat";
    uint8_t* mapped_ = nullptr;
    size_t   mapped_size_ = 0;
    RingBufferHeader* header_ = nullptr;
    uint8_t* data_region_ = nullptr;
    size_t   data_capacity_ = 0;

    std::atomic<bool> running_{false};
    std::vector<std::thread> source_threads_;
    std::mutex record_mutex_;  ///< Protects write_record for safety
};

} // namespace straylight
