// bin/compiler/cache.cpp
#include "cache.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace straylight::compiler {

CompilationCache::CompilationCache(const std::filesystem::path& cache_dir)
    : dir_(cache_dir)
{
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    // If creation fails, operations will fail individually with clear errors.
}

bool CompilationCache::is_valid_hash(const std::string& hash) const {
    if (hash.empty() || hash.size() > 256) return false;
    // Only allow alphanumeric, dash, underscore.
    return std::all_of(hash.begin(), hash.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_';
    });
}

std::filesystem::path CompilationCache::entry_path(const std::string& hash) const {
    return dir_ / (hash + ".slcache");
}

Result<std::string, std::string> CompilationCache::get(
    const std::string& graph_hash) const
{
    if (!is_valid_hash(graph_hash)) {
        return Result<std::string, std::string>::error("invalid cache key");
    }

    auto path = entry_path(graph_hash);
    if (!std::filesystem::exists(path)) {
        return Result<std::string, std::string>::error("cache miss: " + graph_hash);
    }

    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        return Result<std::string, std::string>::error(
            "failed to open cache entry: " + path.string());
    }

    std::ostringstream ss;
    ss << ifs.rdbuf();
    if (ifs.bad()) {
        return Result<std::string, std::string>::error(
            "failed to read cache entry: " + path.string());
    }

    return Result<std::string, std::string>::ok(ss.str());
}

Result<void, std::string> CompilationCache::put(
    const std::string& graph_hash, const std::string& compiled)
{
    if (!is_valid_hash(graph_hash)) {
        return Result<void, std::string>::error("invalid cache key");
    }

    auto path = entry_path(graph_hash);

    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        return Result<void, std::string>::error(
            "failed to create cache entry: " + path.string());
    }

    ofs.write(compiled.data(), static_cast<std::streamsize>(compiled.size()));
    if (!ofs) {
        return Result<void, std::string>::error(
            "failed to write cache entry: " + path.string());
    }

    return Result<void, std::string>::ok();
}

size_t CompilationCache::size() const {
    size_t count = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir_, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".slcache") {
            count++;
        }
    }
    return count;
}

void CompilationCache::clear() {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir_, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".slcache") {
            std::filesystem::remove(entry.path(), ec);
        }
    }
}

} // namespace straylight::compiler
