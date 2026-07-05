// services/ghost/migration_engine.h
// Process migration engine — transparent live migration between machines.
#pragma once

#include "page_server.h"
#include "process_restore.h"

#include <straylight/result.h>
#include <straylight/error.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace straylight {

/// State of a migration.
enum class MigrationState : uint8_t {
    Pending,
    Freezing,
    Capturing,
    Streaming,
    LazyActive,     // Lazy migration: process running, pages faulting in
    Restoring,
    Complete,
    Failed,
    Cancelled,
};

inline const char* migration_state_name(MigrationState s) {
    switch (s) {
        case MigrationState::Pending:    return "pending";
        case MigrationState::Freezing:   return "freezing";
        case MigrationState::Capturing:  return "capturing";
        case MigrationState::Streaming:  return "streaming";
        case MigrationState::LazyActive: return "lazy-active";
        case MigrationState::Restoring:  return "restoring";
        case MigrationState::Complete:   return "complete";
        case MigrationState::Failed:     return "failed";
        case MigrationState::Cancelled:  return "cancelled";
    }
    return "unknown";
}

/// Progress info for a migration.
struct MigrationStatus {
    uint64_t migration_id{0};
    pid_t source_pid{0};
    std::string target_host;
    MigrationState state{MigrationState::Pending};

    uint64_t total_pages{0};
    uint64_t pages_transferred{0};
    uint64_t pages_remaining{0};
    uint64_t hot_pages{0};         // Recently-accessed pages
    uint64_t dirty_pages{0};       // Pages dirtied during migration

    double bandwidth_mbps{0.0};
    double elapsed_seconds{0.0};
    double eta_seconds{0.0};

    std::string error_message;
};

/// Migration options.
struct MigrationOptions {
    bool lazy{false};                 // Lazy migration (page-fault on demand)
    bool compress{true};              // Compress pages with zstd before transfer
    int  compress_level{3};           // zstd compression level
    uint16_t port{9730};             // TCP port for page server
    int  prefetch_lookahead{16};     // Pages to prefetch ahead
    bool transfer_fds{true};         // Transfer open file descriptors
    bool transfer_network{false};    // Attempt to transfer network connections
    int  max_downtime_ms{100};       // Max acceptable freeze time
};

/// The migration engine: coordinates freezing, capturing, streaming, and restoring.
class MigrationEngine {
public:
    MigrationEngine();
    ~MigrationEngine();

    /// Full migration: freeze, capture, stream, restore on target, kill source.
    Result<uint64_t, SLError> migrate(pid_t pid, const std::string& target_host,
                                       const MigrationOptions& opts = {});

    /// Lazy migration: start process on target immediately, fault pages in on demand.
    Result<uint64_t, SLError> lazy_migrate(pid_t pid, const std::string& target_host,
                                            const MigrationOptions& opts = {});

    /// Get status of a migration.
    Result<MigrationStatus, SLError> get_status(uint64_t migration_id) const;

    /// List all migrations (active and recent).
    std::vector<MigrationStatus> list_migrations() const;

    /// Cancel an in-progress migration.
    Result<void, SLError> cancel(uint64_t migration_id);

    /// Handle incoming migration request (target side).
    Result<void, SLError> receive_migration(int connection_fd);

    /// Set the page server for serving pages to remote ghost daemons.
    void set_page_server(PageServer* server) { page_server_ = server; }

private:
    /// Freeze a process (SIGSTOP + wait for stop).
    Result<void, SLError> freeze_process(pid_t pid);

    /// Resume a process (SIGCONT).
    void resume_process(pid_t pid);

    /// Capture process memory maps from /proc/PID/maps.
    Result<std::vector<MemoryRegion>, SLError> capture_memory_maps(pid_t pid);

    /// Capture register state via ptrace.
    Result<RegisterState, SLError> capture_registers(pid_t pid);

    /// Capture open file descriptors from /proc/PID/fd/.
    Result<std::vector<FileDescriptor>, SLError> capture_file_descriptors(pid_t pid);

    /// Read process memory pages, sorted by hotness (recently-accessed first).
    Result<std::vector<PageInfo>, SLError> capture_pages_by_hotness(
        pid_t pid, const std::vector<MemoryRegion>& regions);

    /// Stream pages to the target host over TLS.
    Result<void, SLError> stream_pages(uint64_t migration_id,
                                        pid_t pid,
                                        const std::string& target_host,
                                        const MigrationOptions& opts,
                                        const std::vector<PageInfo>& pages);

    /// Send process image (metadata) to target.
    Result<void, SLError> send_process_image(int conn_fd,
                                              const ProcessImage& image);

    /// Run the migration in a background thread.
    void run_migration(uint64_t migration_id, pid_t pid,
                       const std::string& target_host,
                       MigrationOptions opts, bool lazy);

    /// Update migration status.
    void update_status(uint64_t migration_id,
                       const std::function<void(MigrationStatus&)>& updater);

    mutable std::mutex mutex_;
    std::map<uint64_t, MigrationStatus> migrations_;
    std::map<uint64_t, std::thread> migration_threads_;
    std::atomic<uint64_t> next_id_{1};

    PageServer* page_server_{nullptr};
};

} // namespace straylight
