#pragma once

#include <straylight/result.h>
#include <straylight/error.h>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace straylight {

class Store {
public:
    void set(const std::string& key, std::string value);
    std::optional<std::string> get(const std::string& key) const;
    void del(const std::string& key);
    void watch(const std::string& key,
               std::function<void(const std::string&)> callback);

    std::string serialize() const;   // returns JSON string
    Result<void, SLError> deserialize(const std::string& json);

private:
    std::map<std::string, std::string> data_;
    std::unordered_map<std::string,
        std::vector<std::function<void(const std::string&)>>> watchers_;
    mutable std::shared_mutex mutex_;
};

} // namespace straylight
