// services/whisper/channel.cpp
// Channel and ChannelRegistry implementation.

#include "channel.h"

#include <algorithm>

namespace straylight {

// ---------------------------------------------------------------------------
// Channel
// ---------------------------------------------------------------------------

Channel::Channel(const std::string& name, size_t max_depth)
    : name_(name)
    , max_depth_(max_depth)
    , created_(std::chrono::system_clock::now())
    , last_activity_(created_) {
    acl_.allow_all = true; // Default: open to all.
}

Channel::~Channel() = default;

void Channel::set_acl(const ChannelACL& acl) {
    std::lock_guard<std::mutex> lock(mu_);
    acl_ = acl;
}

bool Channel::is_permitted(uid_t uid, gid_t gid) const {
    std::lock_guard<std::mutex> lock(mu_);
    if (acl_.allow_all) return true;
    if (acl_.allowed_uids.count(uid)) return true;
    if (acl_.allowed_gids.count(gid)) return true;
    return false;
}

Result<void, std::string> Channel::enqueue(WhisperMessage msg) {
    std::lock_guard<std::mutex> lock(mu_);
    if (queue_.size() >= max_depth_) {
        return Result<void, std::string>::error(
            "channel '" + name_ + "' queue full (" +
            std::to_string(max_depth_) + " messages)");
    }
    msg.timestamp = std::chrono::system_clock::now();
    queue_.push_back(std::move(msg));
    last_activity_ = std::chrono::system_clock::now();
    return Result<void, std::string>::ok();
}

Result<WhisperMessage, std::string> Channel::dequeue() {
    std::lock_guard<std::mutex> lock(mu_);
    if (queue_.empty()) {
        return Result<WhisperMessage, std::string>::error(
            "channel '" + name_ + "' is empty");
    }
    WhisperMessage msg = std::move(queue_.front());
    queue_.pop_front();
    last_activity_ = std::chrono::system_clock::now();
    return Result<WhisperMessage, std::string>::ok(std::move(msg));
}

size_t Channel::depth() const {
    std::lock_guard<std::mutex> lock(mu_);
    return queue_.size();
}

bool Channel::idle_for(int seconds) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto now = std::chrono::system_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                       now - last_activity_)
                       .count();
    return elapsed >= seconds;
}

void Channel::touch() {
    std::lock_guard<std::mutex> lock(mu_);
    last_activity_ = std::chrono::system_clock::now();
}

// ---------------------------------------------------------------------------
// ChannelRegistry
// ---------------------------------------------------------------------------

ChannelRegistry::ChannelRegistry() = default;
ChannelRegistry::~ChannelRegistry() = default;

Result<Channel*, std::string>
ChannelRegistry::create(const std::string& name, size_t max_depth) {
    std::lock_guard<std::mutex> lock(mu_);
    if (channels_.count(name)) {
        return Result<Channel*, std::string>::error(
            "channel '" + name + "' already exists");
    }
    auto ch = std::make_unique<Channel>(name, max_depth);
    Channel* ptr = ch.get();
    channels_[name] = std::move(ch);
    return Result<Channel*, std::string>::ok(ptr);
}

Result<Channel*, std::string>
ChannelRegistry::get(const std::string& name) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = channels_.find(name);
    if (it == channels_.end()) {
        return Result<Channel*, std::string>::error(
            "channel '" + name + "' not found");
    }
    return Result<Channel*, std::string>::ok(it->second.get());
}

Result<void, std::string>
ChannelRegistry::remove(const std::string& name) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = channels_.find(name);
    if (it == channels_.end()) {
        return Result<void, std::string>::error(
            "channel '" + name + "' not found");
    }
    channels_.erase(it);
    return Result<void, std::string>::ok();
}

int ChannelRegistry::cleanup_idle(int timeout_secs) {
    std::lock_guard<std::mutex> lock(mu_);
    int removed = 0;
    for (auto it = channels_.begin(); it != channels_.end();) {
        if (it->second->idle_for(timeout_secs)) {
            it = channels_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

std::vector<std::string> ChannelRegistry::list() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::string> names;
    names.reserve(channels_.size());
    for (const auto& [name, _] : channels_) {
        names.push_back(name);
    }
    return names;
}

} // namespace straylight
