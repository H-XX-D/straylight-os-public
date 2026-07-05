// services/whisper/channel.h
// Named message channel for the Whisper encrypted IPC service.
#pragma once

#include <straylight/result.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace straylight {

/// Access control entry for a Whisper channel.
struct ChannelACL {
    std::set<uid_t> allowed_uids;
    std::set<gid_t> allowed_gids;
    bool allow_all = false;
};

/// A single message in the queue.
struct WhisperMessage {
    std::vector<uint8_t> ciphertext;
    std::vector<uint8_t> nonce;
    uid_t sender_uid = 0;
    std::chrono::system_clock::time_point timestamp;
};

/// A named channel with message queue and ACL.
class Channel {
public:
    explicit Channel(const std::string& name, size_t max_depth = 1024);
    ~Channel();

    const std::string& name() const { return name_; }

    /// Set the ACL for this channel.
    void set_acl(const ChannelACL& acl);
    const ChannelACL& acl() const { return acl_; }

    /// Check whether a given UID/GID is permitted.
    bool is_permitted(uid_t uid, gid_t gid) const;

    /// Enqueue a message. Returns error if queue is full.
    Result<void, std::string> enqueue(WhisperMessage msg);

    /// Dequeue the next message, or error if empty.
    Result<WhisperMessage, std::string> dequeue();

    /// Peek at the queue depth.
    size_t depth() const;

    /// Check if the channel has been idle longer than the given seconds.
    bool idle_for(int seconds) const;

    /// Touch the last-activity timestamp.
    void touch();

    /// Get the creation time.
    std::chrono::system_clock::time_point created() const { return created_; }

private:
    std::string name_;
    size_t max_depth_;
    ChannelACL acl_;
    mutable std::mutex mu_;
    std::deque<WhisperMessage> queue_;
    std::chrono::system_clock::time_point created_;
    std::chrono::system_clock::time_point last_activity_;
};

/// Manages all named channels.
class ChannelRegistry {
public:
    ChannelRegistry();
    ~ChannelRegistry();

    /// Create a new named channel.
    Result<Channel*, std::string> create(const std::string& name,
                                          size_t max_depth = 1024);

    /// Get an existing channel by name.
    Result<Channel*, std::string> get(const std::string& name);

    /// Remove a channel.
    Result<void, std::string> remove(const std::string& name);

    /// Remove all channels idle for more than timeout_secs.
    int cleanup_idle(int timeout_secs);

    /// List all channel names.
    std::vector<std::string> list() const;

private:
    mutable std::mutex mu_;
    std::map<std::string, std::unique_ptr<Channel>> channels_;
};

} // namespace straylight
