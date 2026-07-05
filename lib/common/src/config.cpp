// lib/common/src/config.cpp
#include <straylight/config.h>
#include <straylight/log.h>

#include <fstream>
#include <sstream>

namespace straylight {

Result<Config, std::string> Config::load(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        return Result<Config, std::string>::error(
            "Config file not found: " + path.string());
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        return Result<Config, std::string>::error(
            "Cannot open config file: " + path.string());
    }

    try {
        auto data = nlohmann::json::parse(file);
        SL_DEBUG("Loaded config from {}", path.string());
        return Result<Config, std::string>::ok(Config(std::move(data)));
    } catch (const nlohmann::json::parse_error& e) {
        return Result<Config, std::string>::error(
            std::string("JSON parse error: ") + e.what());
    }
}

bool Config::has(std::string_view key) const {
    return resolve(key) != nullptr;
}

const nlohmann::json* Config::resolve(std::string_view dotted_key) const {
    const nlohmann::json* current = &data_;

    std::string key_str(dotted_key);
    std::istringstream stream(key_str);
    std::string segment;

    while (std::getline(stream, segment, '.')) {
        if (!current->is_object() || !current->contains(segment)) {
            return nullptr;
        }
        current = &(*current)[segment];
    }

    return current;
}

} // namespace straylight
