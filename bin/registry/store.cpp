#include "store.h"
#include <nlohmann/json.hpp>

namespace straylight {

void Store::set(const std::string& key, std::string value) {
    // Copy watchers under lock, then invoke outside to avoid deadlock
    // if a callback re-enters set()/del().
    std::vector<std::function<void(const std::string&)>> callbacks;
    {
        std::unique_lock lock(mutex_);
        data_[key] = value;
        if (auto it = watchers_.find(key); it != watchers_.end())
            callbacks = it->second;
    }
    for (auto& cb : callbacks) cb(value);
}

std::optional<std::string> Store::get(const std::string& key) const {
    std::shared_lock lock(mutex_);
    auto it = data_.find(key);
    if (it == data_.end()) return std::nullopt;
    return it->second;
}

void Store::del(const std::string& key) {
    std::unique_lock lock(mutex_);
    data_.erase(key);
    watchers_.erase(key);
}

void Store::watch(const std::string& key,
                  std::function<void(const std::string&)> callback) {
    std::unique_lock lock(mutex_);
    watchers_[key].push_back(std::move(callback));
}

std::string Store::serialize() const {
    std::shared_lock lock(mutex_);
    nlohmann::json j = data_;
    return j.dump(2);
}

Result<void, SLError> Store::deserialize(const std::string& json) {
    try {
        auto j = nlohmann::json::parse(json);
        std::unique_lock lock(mutex_);
        data_ = j.get<std::map<std::string, std::string>>();
        return Result<void, SLError>::ok();
    } catch (const nlohmann::json::exception& e) {
        return Result<void, SLError>::error(
            SLError{SLErrorCode::ParseError, e.what()});
    }
}

// notify_watchers removed — watcher invocation is now inlined in set()
// to avoid TOCTOU race (del() could erase watchers between set's unlock
// and notify's re-lock).

} // namespace straylight
