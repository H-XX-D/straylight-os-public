// lib/common/include/straylight/config.h
#pragma once

#include <straylight/export.h>
#include <straylight/result.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <string_view>

namespace straylight {

/// JSON-based configuration loaded from file.
/// Supports dot-notation access for nested keys ("display.width").
class STRAYLIGHT_EXPORT Config {
public:
    /// Load a JSON config file. Returns error string on failure.
    static Result<Config, std::string> load(const std::filesystem::path& path);

    /// Get a typed value by key. Supports dot-notation for nesting.
    /// Throws if key not found and no default provided.
    template <typename T>
    T get(std::string_view key) const {
        const auto* node = resolve(key);
        if (!node || node->is_null()) {
            throw std::runtime_error(
                std::string("Config key not found: ") + std::string(key));
        }
        return node->get<T>();
    }

    /// Get a typed value with a default fallback.
    template <typename T>
    T get(std::string_view key, T default_val) const {
        const auto* node = resolve(key);
        if (!node || node->is_null()) {
            return default_val;
        }
        return node->get<T>();
    }

    /// Check if a key exists.
    [[nodiscard]] bool has(std::string_view key) const;

    /// Get the raw JSON object.
    [[nodiscard]] const nlohmann::json& raw() const { return data_; }

    /// Create an empty config with no keys (for default-initialized daemons).
    [[nodiscard]] static Config make_empty() {
        return Config(nlohmann::json::object());
    }

private:
    explicit Config(nlohmann::json data) : data_(std::move(data)) {}
    const nlohmann::json* resolve(std::string_view dotted_key) const;

    nlohmann::json data_;
};

} // namespace straylight
