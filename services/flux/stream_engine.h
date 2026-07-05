// services/flux/stream_engine.h
// Named stream management with ring buffers, subscriptions, and replay.
#pragma once

#include <straylight/result.h>
#include <straylight/error.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace straylight {

/// A single event in a stream — timestamped JSON payload.
struct StreamEvent {
    uint64_t sequence = 0;
    std::chrono::steady_clock::time_point timestamp;
    std::chrono::system_clock::time_point wall_timestamp;
    nlohmann::json payload;
};

/// Ring buffer holding the last N events for a single named stream.
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity) : capacity_(capacity) {}

    void push(const StreamEvent& event) {
        std::lock_guard lock(mu_);
        if (buffer_.size() >= capacity_) {
            buffer_.pop_front();
        }
        buffer_.push_back(event);
    }

    std::vector<StreamEvent> last_n(size_t n) const {
        std::lock_guard lock(mu_);
        std::vector<StreamEvent> result;
        size_t start = (n >= buffer_.size()) ? 0 : buffer_.size() - n;
        for (size_t i = start; i < buffer_.size(); ++i) {
            result.push_back(buffer_[i]);
        }
        return result;
    }

    std::vector<StreamEvent> since_sequence(uint64_t seq) const {
        std::lock_guard lock(mu_);
        std::vector<StreamEvent> result;
        for (const auto& ev : buffer_) {
            if (ev.sequence > seq) {
                result.push_back(ev);
            }
        }
        return result;
    }

    size_t size() const {
        std::lock_guard lock(mu_);
        return buffer_.size();
    }

    size_t capacity() const { return capacity_; }

private:
    mutable std::mutex mu_;
    std::deque<StreamEvent> buffer_;
    size_t capacity_;
};

/// Callback type for stream subscribers.
using SubscriberCallback = std::function<void(const StreamEvent&)>;

/// A subscriber registration with an optional filter.
struct Subscriber {
    uint64_t id = 0;
    std::string filter_expr;
    SubscriberCallback callback;
};

/// Metadata about a named stream.
struct StreamInfo {
    std::string name;
    size_t buffer_capacity = 1000;
    uint64_t total_published = 0;
    size_t subscriber_count = 0;
    std::chrono::steady_clock::time_point created_at;
};

/// Manages all named streams: creation, publishing, subscribing, filtering.
class StreamEngine {
public:
    StreamEngine() = default;
    ~StreamEngine() = default;

    /// Create a new named stream with the given ring buffer capacity.
    Result<void, SLError> create_stream(const std::string& name, size_t buffer_capacity);

    /// Delete a named stream and disconnect all subscribers.
    Result<void, SLError> delete_stream(const std::string& name);

    /// Publish a JSON payload to a named stream. Notifies all subscribers.
    Result<void, SLError> publish(const std::string& stream_name, const nlohmann::json& payload);

    /// Subscribe to a stream with an optional filter expression.
    /// Returns a subscriber ID that can be used to unsubscribe.
    Result<uint64_t, SLError> subscribe(const std::string& stream_name,
                                         SubscriberCallback callback,
                                         const std::string& filter_expr = "");

    /// Unsubscribe from a stream by subscriber ID.
    Result<void, SLError> unsubscribe(const std::string& stream_name, uint64_t subscriber_id);

    /// Get the last N events from a stream (for replay / late join).
    Result<std::vector<StreamEvent>, SLError> replay(const std::string& stream_name, size_t count);

    /// Get events since a given sequence number.
    Result<std::vector<StreamEvent>, SLError> since(const std::string& stream_name, uint64_t seq);

    /// List all streams with metadata.
    std::vector<StreamInfo> list_streams() const;

    /// Check if a stream exists.
    bool has_stream(const std::string& name) const;

    /// Get info for a single stream.
    Result<StreamInfo, SLError> stream_info(const std::string& name) const;

private:
    struct Stream {
        std::string name;
        RingBuffer buffer;
        std::vector<Subscriber> subscribers;
        uint64_t next_sequence = 1;
        uint64_t next_subscriber_id = 1;
        std::chrono::steady_clock::time_point created_at;
        mutable std::mutex sub_mu;

        explicit Stream(const std::string& n, size_t cap)
            : name(n), buffer(cap), created_at(std::chrono::steady_clock::now()) {}
    };

    mutable std::shared_mutex mu_;
    std::unordered_map<std::string, std::unique_ptr<Stream>> streams_;
};

} // namespace straylight
