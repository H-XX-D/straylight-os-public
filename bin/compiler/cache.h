// bin/compiler/cache.h
#pragma once

#include <straylight/result.h>

#include <cstddef>
#include <filesystem>
#include <string>

namespace straylight::compiler {

/// File-system backed compilation cache.
/// Each entry is stored as a file named by the graph hash, containing the
/// compiled IR as its content.
class CompilationCache {
public:
    /// Construct a cache rooted at the given directory.
    /// The directory will be created if it does not exist.
    explicit CompilationCache(const std::filesystem::path& cache_dir);

    /// Look up a cached compilation result by graph hash.
    /// Returns the compiled IR string, or an error if not found.
    Result<std::string, std::string> get(const std::string& graph_hash) const;

    /// Store a compiled IR string under the given graph hash.
    Result<void, std::string> put(const std::string& graph_hash,
                                   const std::string& compiled);

    /// Return the number of entries in the cache.
    size_t size() const;

    /// Remove all entries from the cache.
    void clear();

private:
    std::filesystem::path dir_;

    /// Validate that a hash string is safe to use as a filename.
    bool is_valid_hash(const std::string& hash) const;

    /// Build the full path for a cache entry.
    std::filesystem::path entry_path(const std::string& hash) const;
};

} // namespace straylight::compiler
