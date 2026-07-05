// tools/trace/tracer.h
// Syscall-level application tracer using ptrace or eBPF.
#pragma once

#include <straylight/result.h>
#include <straylight/error.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace straylight {

/// A single captured syscall event.
struct SyscallEvent {
    uint64_t timestamp_ns = 0;      // Monotonic nanoseconds since trace start
    pid_t pid = 0;
    pid_t tid = 0;
    int syscall_nr = 0;
    std::string syscall_name;
    int64_t return_value = 0;
    uint64_t duration_ns = 0;       // Time spent inside the syscall
    std::vector<uint64_t> args;     // Up to 6 syscall arguments
};

/// Aggregated file I/O activity for a path.
struct FileIORecord {
    std::string path;
    uint64_t read_bytes = 0;
    uint64_t write_bytes = 0;
    uint64_t read_calls = 0;
    uint64_t write_calls = 0;
    uint64_t total_latency_ns = 0;  // Total time in I/O syscalls for this file
    uint64_t open_count = 0;
};

/// Aggregated network connection record.
struct NetworkRecord {
    std::string remote_addr;        // IP:port or socket path
    std::string local_addr;
    std::string protocol;           // "tcp", "udp", "unix"
    uint64_t bytes_sent = 0;
    uint64_t bytes_received = 0;
    uint64_t connect_latency_ns = 0;
};

/// Memory operation record.
struct MemoryRecord {
    uint64_t mmap_calls = 0;
    uint64_t mmap_total_bytes = 0;
    uint64_t munmap_calls = 0;
    uint64_t brk_calls = 0;
    uint64_t peak_brk = 0;
    uint64_t mprotect_calls = 0;
};

/// Signal record.
struct SignalRecord {
    int signal_nr = 0;
    std::string signal_name;
    uint64_t count = 0;
    uint64_t last_timestamp_ns = 0;
};

/// Complete trace data for a session.
struct TraceData {
    pid_t traced_pid = 0;
    std::string command;
    uint64_t start_time_ns = 0;
    uint64_t end_time_ns = 0;
    int exit_code = -1;

    std::vector<SyscallEvent> events;

    // Aggregated data
    std::unordered_map<std::string, uint64_t> syscall_counts;
    std::unordered_map<std::string, uint64_t> syscall_total_time_ns;
    std::vector<FileIORecord> file_io;
    std::vector<NetworkRecord> network;
    MemoryRecord memory;
    std::vector<SignalRecord> signals;

    uint64_t total_syscalls = 0;
    uint64_t total_duration_ns = 0;
};

/// Callback for live event reporting.
using TraceCallback = std::function<void(const SyscallEvent&)>;

/// Traces a process's syscalls, file access, network, and memory operations.
class Tracer {
public:
    Tracer();
    ~Tracer();

    /// Trace a new command from start to exit.
    /// Forks, execs the command under ptrace, collects all events.
    Result<TraceData, SLError> run(const std::vector<std::string>& argv);

    /// Attach to an already-running process.
    Result<TraceData, SLError> attach(pid_t pid);

    /// Stop the current trace (for attach mode).
    void stop();

    /// Set a callback for live event reporting.
    void set_callback(TraceCallback cb);

    /// Enable/disable recording of individual events (memory intensive).
    void set_record_events(bool enable) { record_events_ = enable; }

    /// Set maximum events to record (0 = unlimited).
    void set_max_events(size_t max) { max_events_ = max; }

private:
    /// Internal trace loop — runs in the parent after fork or attach.
    Result<TraceData, SLError> trace_loop(pid_t child_pid, const std::string& cmd_str);

    /// Resolve syscall number to name.
    std::string syscall_name(int nr) const;

    /// Parse file descriptor to path (from /proc/pid/fd/).
    std::string fd_to_path(pid_t pid, int fd) const;

    /// Classify and aggregate a syscall event into TraceData.
    void classify_event(TraceData& data, const SyscallEvent& event);

    /// Build the syscall number -> name table.
    void init_syscall_table();

    TraceCallback callback_;
    std::atomic<bool> stop_requested_{false};
    bool record_events_ = true;
    size_t max_events_ = 0;

    std::unordered_map<int, std::string> syscall_table_;
    std::unordered_map<int, std::string> fd_path_cache_;
    std::mutex fd_cache_mu_;
};

} // namespace straylight
