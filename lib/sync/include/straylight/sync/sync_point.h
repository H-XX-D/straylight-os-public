/**
 * StrayLight Sync Kernel — SyncPoint (Barrier)
 *
 * Named N-way barrier where N participants must all arrive
 * before any can proceed. Inspired by NVIDIA's cooperative
 * group sync and CUDA's __syncthreads().
 *
 * Use cases:
 *   - Boot phases: all critical services check in before phase advances
 *   - Compositor: all render passes complete before present
 *   - Testing: synchronize parallel test workers
 */
#pragma once

#include <straylight/result.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace straylight::sync {

/// Barrier state.
enum class BarrierState : uint8_t {
    Open,       // Accepting arrivals
    Reached,    // All participants arrived — barrier released
    Expired,    // Timed out before all arrived
    Cancelled   // Explicitly cancelled
};

inline const char* barrier_state_str(BarrierState s) {
    switch (s) {
        case BarrierState::Open:      return "open";
        case BarrierState::Reached:   return "reached";
        case BarrierState::Expired:   return "expired";
        case BarrierState::Cancelled: return "cancelled";
    }
    return "unknown";
}

/// SyncPoint creation info.
struct SyncPointCreateInfo {
    std::string name;
    uint32_t participant_count = 0;  // How many must arrive
    int timeout_ms = -1;             // Auto-expire (-1 = never)
    bool auto_reset = false;         // Reset after all arrive (reusable barrier)
};

/// Info about who arrived at the sync point.
struct Arrival {
    std::string participant;
    std::chrono::steady_clock::time_point arrived_at;
};

/// An N-way sync point / barrier.
///
/// Works like CUDA's __syncthreads() but for StrayLight services.
/// A boot phase can declare "wait for all 5 critical services to be ready"
/// and each service calls arrive() when it's up. Once all 5 arrive,
/// the barrier opens and everyone proceeds.
class SyncPoint {
public:
    SyncPoint();
    ~SyncPoint();

    SyncPoint(const SyncPoint&) = delete;
    SyncPoint& operator=(const SyncPoint&) = delete;

    /// Create a new sync point.
    static Result<SyncPoint, std::string> create(const SyncPointCreateInfo& info);

    /// A participant arrives at this barrier.
    /// Blocks until all participants arrive (or timeout).
    /// @param participant  Name of the arriving entity (for debug)
    VoidResult<std::string> arrive_and_wait(const std::string& participant,
                                              int timeout_ms = -1);

    /// Arrive without waiting — fire and forget.
    /// Returns immediately, others still wait for this participant.
    VoidResult<std::string> arrive(const std::string& participant);

    /// Wait without arriving — observe the barrier.
    VoidResult<std::string> wait(int timeout_ms = -1);

    /// Cancel the barrier — releases all waiters with error.
    void cancel();

    /// Reset the barrier for reuse (only if auto_reset is off).
    VoidResult<std::string> reset();

    /// Dynamically adjust participant count (before barrier is reached).
    VoidResult<std::string> set_participant_count(uint32_t count);

    /// Current state.
    [[nodiscard]] BarrierState state() const;

    /// How many have arrived.
    [[nodiscard]] uint32_t arrived_count() const;

    /// Total expected.
    [[nodiscard]] uint32_t participant_count() const { return expected_; }

    /// Who has arrived so far.
    std::vector<Arrival> arrivals() const;

    /// Name.
    [[nodiscard]] const std::string& name() const { return name_; }

private:
    std::string name_;
    uint32_t expected_ = 0;
    bool auto_reset_ = false;
    int default_timeout_ms_ = -1;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    BarrierState state_ = BarrierState::Open;
    std::vector<Arrival> arrivals_;
};

} // namespace straylight::sync
