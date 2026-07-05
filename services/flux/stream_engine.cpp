// services/flux/stream_engine.cpp
#include "stream_engine.h"
#include "filters.h"
#include <straylight/log.h>

namespace straylight {

Result<void, SLError> StreamEngine::create_stream(const std::string& name, size_t buffer_capacity) {
    std::unique_lock lock(mu_);

    if (streams_.count(name)) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::AlreadyExists, "Stream '" + name + "' already exists"});
    }

    streams_[name] = std::make_unique<Stream>(name, buffer_capacity);
    SL_INFO("flux: created stream '{}' (buffer={})", name, buffer_capacity);
    return Result<void, SLError>::ok();
}

Result<void, SLError> StreamEngine::delete_stream(const std::string& name) {
    std::unique_lock lock(mu_);

    auto it = streams_.find(name);
    if (it == streams_.end()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::NotFound, "Stream '" + name + "' not found"});
    }

    streams_.erase(it);
    SL_INFO("flux: deleted stream '{}'", name);
    return Result<void, SLError>::ok();
}

Result<void, SLError> StreamEngine::publish(const std::string& stream_name,
                                             const nlohmann::json& payload) {
    std::shared_lock lock(mu_);

    auto it = streams_.find(stream_name);
    if (it == streams_.end()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::NotFound, "Stream '" + stream_name + "' not found"});
    }

    auto& stream = *it->second;

    StreamEvent event;
    event.timestamp = std::chrono::steady_clock::now();
    event.wall_timestamp = std::chrono::system_clock::now();
    event.payload = payload;

    {
        std::lock_guard sub_lock(stream.sub_mu);
        event.sequence = stream.next_sequence++;
    }

    stream.buffer.push(event);

    // Notify subscribers — evaluate filters and dispatch
    std::lock_guard sub_lock(stream.sub_mu);
    for (const auto& sub : stream.subscribers) {
        bool passes = true;
        if (!sub.filter_expr.empty()) {
            auto parsed = FilterEngine::parse(sub.filter_expr);
            if (parsed.has_value()) {
                passes = FilterEngine::evaluate(parsed.value(), payload);
            }
        }
        if (passes && sub.callback) {
            sub.callback(event);
        }
    }

    SL_TRACE("flux: published seq={} to stream '{}'", event.sequence, stream_name);
    return Result<void, SLError>::ok();
}

Result<uint64_t, SLError> StreamEngine::subscribe(const std::string& stream_name,
                                                    SubscriberCallback callback,
                                                    const std::string& filter_expr) {
    std::shared_lock lock(mu_);

    auto it = streams_.find(stream_name);
    if (it == streams_.end()) {
        return Result<uint64_t, SLError>::error(
            SLError{SLErrorCode::NotFound, "Stream '" + stream_name + "' not found"});
    }

    auto& stream = *it->second;

    // Validate filter expression if provided
    if (!filter_expr.empty()) {
        auto parsed = FilterEngine::parse(filter_expr);
        if (!parsed.has_value()) {
            return Result<uint64_t, SLError>::error(parsed.error());
        }
    }

    std::lock_guard sub_lock(stream.sub_mu);

    Subscriber sub;
    sub.id = stream.next_subscriber_id++;
    sub.filter_expr = filter_expr;
    sub.callback = std::move(callback);

    uint64_t id = sub.id;
    stream.subscribers.push_back(std::move(sub));

    SL_INFO("flux: subscriber {} joined stream '{}' (filter='{}')", id, stream_name, filter_expr);
    return Result<uint64_t, SLError>::ok(id);
}

Result<void, SLError> StreamEngine::unsubscribe(const std::string& stream_name,
                                                  uint64_t subscriber_id) {
    std::shared_lock lock(mu_);

    auto it = streams_.find(stream_name);
    if (it == streams_.end()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::NotFound, "Stream '" + stream_name + "' not found"});
    }

    auto& stream = *it->second;
    std::lock_guard sub_lock(stream.sub_mu);

    auto& subs = stream.subscribers;
    auto sit = std::find_if(subs.begin(), subs.end(),
                            [subscriber_id](const Subscriber& s) { return s.id == subscriber_id; });
    if (sit == subs.end()) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::NotFound, "Subscriber " + std::to_string(subscriber_id) + " not found"});
    }

    subs.erase(sit);
    SL_INFO("flux: subscriber {} left stream '{}'", subscriber_id, stream_name);
    return Result<void, SLError>::ok();
}

Result<std::vector<StreamEvent>, SLError> StreamEngine::replay(const std::string& stream_name,
                                                                 size_t count) {
    std::shared_lock lock(mu_);

    auto it = streams_.find(stream_name);
    if (it == streams_.end()) {
        return Result<std::vector<StreamEvent>, SLError>::error(
            SLError{SLErrorCode::NotFound, "Stream '" + stream_name + "' not found"});
    }

    return Result<std::vector<StreamEvent>, SLError>::ok(it->second->buffer.last_n(count));
}

Result<std::vector<StreamEvent>, SLError> StreamEngine::since(const std::string& stream_name,
                                                                uint64_t seq) {
    std::shared_lock lock(mu_);

    auto it = streams_.find(stream_name);
    if (it == streams_.end()) {
        return Result<std::vector<StreamEvent>, SLError>::error(
            SLError{SLErrorCode::NotFound, "Stream '" + stream_name + "' not found"});
    }

    return Result<std::vector<StreamEvent>, SLError>::ok(it->second->buffer.since_sequence(seq));
}

std::vector<StreamInfo> StreamEngine::list_streams() const {
    std::shared_lock lock(mu_);
    std::vector<StreamInfo> result;
    result.reserve(streams_.size());

    for (const auto& [name, stream] : streams_) {
        StreamInfo info;
        info.name = name;
        info.buffer_capacity = stream->buffer.capacity();
        info.created_at = stream->created_at;

        std::lock_guard sub_lock(stream->sub_mu);
        info.total_published = stream->next_sequence - 1;
        info.subscriber_count = stream->subscribers.size();
        result.push_back(info);
    }

    return result;
}

bool StreamEngine::has_stream(const std::string& name) const {
    std::shared_lock lock(mu_);
    return streams_.count(name) > 0;
}

Result<StreamInfo, SLError> StreamEngine::stream_info(const std::string& name) const {
    std::shared_lock lock(mu_);

    auto it = streams_.find(name);
    if (it == streams_.end()) {
        return Result<StreamInfo, SLError>::error(
            SLError{SLErrorCode::NotFound, "Stream '" + name + "' not found"});
    }

    const auto& stream = *it->second;
    StreamInfo info;
    info.name = stream.name;
    info.buffer_capacity = stream.buffer.capacity();
    info.created_at = stream.created_at;

    std::lock_guard sub_lock(stream.sub_mu);
    info.total_published = stream.next_sequence - 1;
    info.subscriber_count = stream.subscribers.size();

    return Result<StreamInfo, SLError>::ok(info);
}

} // namespace straylight
