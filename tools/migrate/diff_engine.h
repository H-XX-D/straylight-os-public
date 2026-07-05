// tools/migrate/diff_engine.h
// Differential comparison engine for StrayLight system configurations.
#pragma once

#include <straylight/result.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace straylight {

/// A single file difference.
struct FileDiff {
    std::string path;
    std::string status;     // "added", "removed", "modified", "unchanged"
    uint64_t local_size = 0;
    uint64_t remote_size = 0;
    std::string local_hash;   // SHA-256
    std::string remote_hash;
    std::string local_mtime;  // ISO 8601
    std::string remote_mtime;
};

/// Summary of differences between two systems.
struct DiffSummary {
    std::vector<FileDiff> files;
    int added = 0;
    int removed = 0;
    int modified = 0;
    int unchanged = 0;
    uint64_t transfer_bytes = 0;  // Total bytes that need transfer
};

/// A file manifest entry for a StrayLight system.
struct ManifestEntry {
    std::string path;
    uint64_t size = 0;
    std::string sha256;
    std::string mtime;
};

/// Computes differences between local and remote system configurations.
class DiffEngine {
public:
    /// Build a manifest of all StrayLight configuration files on the local system.
    Result<std::vector<ManifestEntry>, std::string> build_local_manifest() const;

    /// Compare local manifest against a remote manifest.
    DiffSummary compare(const std::vector<ManifestEntry>& local,
                        const std::vector<ManifestEntry>& remote) const;

    /// Serialize a manifest to JSON.
    static std::string manifest_to_json(const std::vector<ManifestEntry>& manifest);

    /// Deserialize a manifest from JSON.
    static Result<std::vector<ManifestEntry>, std::string> manifest_from_json(
        const std::string& json_str);

    /// Format a diff summary as a human-readable report.
    static std::string format_diff(const DiffSummary& diff);

private:
    /// Hash a file with SHA-256.
    static Result<std::string, std::string> hash_file(const std::filesystem::path& path);

    /// Get file modification time as ISO 8601.
    static std::string file_mtime(const std::filesystem::path& path);

    /// Enumerate all StrayLight config paths.
    static std::vector<std::filesystem::path> config_paths();
};

} // namespace straylight
