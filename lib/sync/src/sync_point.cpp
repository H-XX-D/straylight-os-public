/**
 * StrayLight Sync Kernel — SyncPoint implementation
 *
 * N-way barrier using condition_variable for in-process sync.
 * For cross-process barriers, the SyncDaemon wraps this with
 * eventfd-based notification over Unix sockets.
 */
#include "straylight/sync/sync_point.h"

#include <algorithm>

namespace straylight::sync {

SyncPoint::SyncPoint() = default;
SyncPoint::~SyncPoint() = default;

Result<SyncPoint, std::string> SyncPoint::create(const SyncPointCreateInfo& info) {
    if (info.participant_count == 0) {
        return Result<SyncPoint, std::string>::error(
            "SyncPoint requires at least 1 participant");
    }

    SyncPoint sp;
    sp.name_ = info.name.empty() ? "syncpoint" : info.name;
    sp.expected_ = info.participant_count;
    sp.auto_reset_ = info.auto_reset;
    sp.default_timeout_ms_ = info.timeout_ms;
    sp.state_ = BarrierState::Open;

    return Result<SyncPoint, std::string>::ok(std::move(sp));
}

VoidResult<std::string> SyncPoint::arrive_and_wait(
    const std::string& participant, int timeout_ms)
{
    std::unique_lock<std::mutex> lock(mutex_);

    if (state_ != BarrierState::Open) {
        return VoidResult<std::string>::error(
            "SyncPoint '" + name_ + "' is not open (state: "
            + barrier_state_str(state_) + ")");
    }

    // Check for duplicate arrival
    for (const auto& a : arrivals_) {
        if (a.participant == participant) {
            return VoidResult<std::string>::error(
                "Participant '" + participant + "' already arrived");
        }
    }

    // Record arrival
    arrivals_.push_back({participant, std::chrono::steady_clock::now()});

    // Check if barrier is complete
    if (arrivals_.size() >= expected_) {
        state_ = BarrierState::Reached;
        cv_.notify_all();
        return VoidResult<std::string>::ok();
    }

    // Wait for remaining participants
    int wait_ms = timeout_ms >= 0 ? timeout_ms : default_timeout_ms_;

    if (wait_ms >= 0) {
        auto result = cv_.wait_for(lock, std::chrono::milliseconds(wait_ms),
            [this] { return state_ != BarrierState::Open; });

        if (!result) {
            state_ = BarrierState::Expired;
            cv_.notify_all();
            return VoidResult<std::string>::error(
                "SyncPoint '" + name_ + "' timed out ("
                + std::to_string(arrivals_.size()) + "/"
                + std::to_string(expected_) + " arrived)");
        }
    } else {
        cv_.wait(lock, [this] { return state_ != BarrierState::Open; });
    }

    if (state_ == BarrierState::Cancelled) {
        return VoidResult<std::string>::error(
            "SyncPoint '" + name_ + "' was cancelled");
    }

    // Auto-reset if configured
    if (auto_reset_ && state_ == BarrierState::Reached) {
        state_ = BarrierState::Open;
        arrivals_.clear();
    }

    return VoidResult<std::string>::ok();
}

VoidResult<std::string> SyncPoint::arrive(const std::string& participant) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (state_ != BarrierState::Open) {
        return VoidResult<std::string>::error(
            "SyncPoint '" + name_ + "' is not open");
    }

    for (const auto& a : arrivals_) {
        if (a.participant == participant) {
            return VoidResult<std::string>::error(
                "Participant '" + participant + "' already arrived");
        }
    }

    arrivals_.push_back({participant, std::chrono::steady_clock::now()});

    if (arrivals_.size() >= expected_) {
        state_ = BarrierState::Reached;
        cv_.notify_all();
    }

    return VoidResult<std::string>::ok();
}

VoidResult<std::string> SyncPoint::wait(int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);

    if (state_ == BarrierState::Reached) {
        return VoidResult<std::string>::ok();
    }

    int wait_ms = timeout_ms >= 0 ? timeout_ms : default_timeout_ms_;

    if (wait_ms >= 0) {
        auto result = cv_.wait_for(lock, std::chrono::milliseconds(wait_ms),
            [this] { return state_ != BarrierState::Open; });
        if (!result) {
            return VoidResult<std::string>::error("SyncPoint wait timed out");
        }
    } else {
        cv_.wait(lock, [this] { return state_ != BarrierState::Open; });
    }

    return state_ == BarrierState::Reached
        ? VoidResult<std::string>::ok()
        : VoidResult<std::string>::error("SyncPoint did not reach");
}

void SyncPoint::cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = BarrierState::Cancelled;
    cv_.notify_all();
}

VoidResult<std::string> SyncPoint::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == BarrierState::Open) {
        return VoidResult<std::string>::error("SyncPoint is still open");
    }
    state_ = BarrierState::Open;
    arrivals_.clear();
    return VoidResult<std::string>::ok();
}

VoidResult<std::string> SyncPoint::set_participant_count(uint32_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != BarrierState::Open) {
        return VoidResult<std::string>::error("Cannot change count after barrier is triggered");
    }
    if (count == 0) {
        return VoidResult<std::string>::error("Count must be > 0");
    }
    expected_ = count;

    // Check if we already have enough arrivals
    if (arrivals_.size() >= expected_) {
        state_ = BarrierState::Reached;
        cv_.notify_all();
    }

    return VoidResult<std::string>::ok();
}

BarrierState SyncPoint::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

uint32_t SyncPoint::arrived_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<uint32_t>(arrivals_.size());
}

std::vector<Arrival> SyncPoint::arrivals() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return arrivals_;
}

} // namespace straylight::sync
