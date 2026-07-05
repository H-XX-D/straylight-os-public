/**
 * StrayLight Mirror Engine — Live system cloning core.
 *
 * Streams entire OS state to another machine over the network:
 *   Phase 1: Filesystem sync (rsync-like delta algorithm)
 *   Phase 2: Service state capture (DaemonBase snapshots)
 *   Phase 3: VPU memory snapshot (slab metadata)
 *   Phase 4: Final sync + cutover signal
 */
#pragma once

#include "state_capture.h"
#include "transfer.h"
#include "straylight/result.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace straylight::mirror {

/// Mirror session phase.
enum class MirrorPhase {
    Idle,
    FilesystemSync,
    ServiceCapture,
    VpuSnapshot,
    FinalSync,
    Complete,
    Failed
};

inline const char* phase_to_string(MirrorPhase p) {
    switch (p) {
        case MirrorPhase::Idle:            return "idle";
        case MirrorPhase::FilesystemSync:  return "filesystem_sync";
        case MirrorPhase::ServiceCapture:  return "service_capture";
        case MirrorPhase::VpuSnapshot:     return "vpu_snapshot";
        case MirrorPhase::FinalSync:       return "final_sync";
        case MirrorPhase::Complete:        return "complete";
        case MirrorPhase::Failed:          return "failed";
    }
    return "unknown";
}

/// Mirror session role.
enum class MirrorRole {
    Source,     // This machine is being cloned
    Target      // This machine receives the clone
};

/// Mirror session progress.
struct MirrorProgress {
    MirrorPhase phase = MirrorPhase::Idle;
    uint64_t total_bytes = 0;
    uint64_t synced_bytes = 0;
    uint64_t files_synced = 0;
    uint64_t files_total = 0;
    double elapsed_seconds = 0.0;
    std::string error;

    double percent_complete() const {
        if (total_bytes == 0) return 0.0;
        return (static_cast<double>(synced_bytes) /
                static_cast<double>(total_bytes)) * 100.0;
    }
};

/// Block checksum for delta sync.
struct BlockChecksum {
    uint64_t offset;
    uint32_t size;
    uint32_t weak_checksum;     // Rolling Adler32-like
    uint32_t strong_checksum;   // CRC32
};

/// File delta — blocks that differ between source and target.
struct FileDelta {
    std::string path;
    uint64_t file_size;
    std::vector<uint64_t> changed_block_offsets;
    std::vector<std::vector<uint8_t>> changed_block_data;
};

class MirrorEngine {
public:
    MirrorEngine();
    ~MirrorEngine();

    /// Start a mirror session as source (push to target).
    VoidResult<std::string> start_mirror(const std::string& target_host,
                                          uint16_t target_port);

    /// Start listening for an incoming mirror session (target mode).
    VoidResult<std::string> start_target(uint16_t listen_port);

    /// Stop the current mirror session.
    void stop_mirror();

    /// Get current progress.
    MirrorProgress get_progress() const;

    /// Check if a mirror session is active.
    [[nodiscard]] bool is_active() const { return active_.load(); }

    /// Set bandwidth limit.
    void set_bandwidth_limit_mbps(double mbps) { bandwidth_limit_mbps_ = mbps; }

    /// Set filesystem paths to sync.
    void set_sync_paths(const std::vector<std::string>& paths) { sync_paths_ = paths; }

    /// Verify the mirror was successful (compare checksums).
    Result<bool, std::string> verify() const;

private:
    std::atomic<bool> active_{false};
    std::atomic<bool> stop_requested_{false};
    MirrorRole role_ = MirrorRole::Source;
    double bandwidth_limit_mbps_ = 0.0;
    std::vector<std::string> sync_paths_;

    Transfer transfer_;
    mutable std::mutex progress_mutex_;
    MirrorProgress progress_;
    std::thread worker_thread_;
    std::chrono::steady_clock::time_point start_time_;

    /// Source workflow.
    void source_workflow(const std::string& target_host, uint16_t target_port);

    /// Target workflow.
    void target_workflow(uint16_t listen_port);

    /// Phase 1: Filesystem sync.
    VoidResult<std::string> phase1_filesystem_sync();

    /// Phase 2: Service state capture and transfer.
    VoidResult<std::string> phase2_service_capture();

    /// Phase 3: VPU snapshot and transfer.
    VoidResult<std::string> phase3_vpu_snapshot();

    /// Phase 4: Final sync and cutover.
    VoidResult<std::string> phase4_final_sync();

    /// Compute block checksums for a file.
    static std::vector<BlockChecksum> compute_block_checksums(
        const std::string& path, uint32_t block_size = 65536);

    /// Compute delta between local and remote checksums.
    static FileDelta compute_delta(const std::string& path,
                                    const std::vector<BlockChecksum>& remote_checksums,
                                    uint32_t block_size = 65536);

    /// Compute a weak rolling checksum (Adler32-like).
    static uint32_t weak_checksum(const uint8_t* data, size_t len);

    /// Enumerate all files in the sync paths.
    std::vector<std::string> enumerate_files() const;

    /// Update progress.
    void set_phase(MirrorPhase phase);
    void update_progress(uint64_t synced_bytes, uint64_t files_synced);
};

} // namespace straylight::mirror
