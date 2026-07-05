// tools/migrate/diff_engine.cpp
// Differential comparison engine implementation.
#include "diff_engine.h"

#include <nlohmann/json.hpp>
#include <openssl/sha.h>

#include <algorithm>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>

namespace straylight {

// ---------------------------------------------------------------------------
// Config paths
// ---------------------------------------------------------------------------

std::vector<std::filesystem::path> DiffEngine::config_paths() {
    std::vector<std::filesystem::path> paths;

    // System config
    paths.emplace_back("/etc/straylight");

    // User config
    const char* home = std::getenv("HOME");
    if (home) {
        paths.emplace_back(std::string(home) + "/.config/straylight");
        paths.emplace_back(std::string(home) + "/.ssh");
        paths.emplace_back(std::string(home) + "/.local/share/straylight");
    }

    return paths;
}

// ---------------------------------------------------------------------------
// File hashing
// ---------------------------------------------------------------------------

Result<std::string, std::string> DiffEngine::hash_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return Result<std::string, std::string>::error("Cannot open: " + path.string());
    }

    SHA256_CTX ctx;
    SHA256_Init(&ctx);

    char buf[8192];
    while (in.read(buf, sizeof(buf))) {
        SHA256_Update(&ctx, buf, static_cast<size_t>(in.gcount()));
    }
    if (in.gcount() > 0) {
        SHA256_Update(&ctx, buf, static_cast<size_t>(in.gcount()));
    }

    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_Final(hash, &ctx);

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        oss << std::setw(2) << static_cast<int>(hash[i]);
    }
    return Result<std::string, std::string>::ok(oss.str());
}

std::string DiffEngine::file_mtime(const std::filesystem::path& path) {
    std::error_code ec;
    auto ftime = std::filesystem::last_write_time(path, ec);
    if (ec) return "";

    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - std::filesystem::file_time_type::clock::now() +
        std::chrono::system_clock::now());
    auto t = std::chrono::system_clock::to_time_t(sctp);
    std::tm tm_buf{};
    gmtime_r(&t, &tm_buf);

    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%FT%TZ");
    return oss.str();
}

// ---------------------------------------------------------------------------
// Manifest building
// ---------------------------------------------------------------------------

Result<std::vector<ManifestEntry>, std::string> DiffEngine::build_local_manifest() const {
    std::vector<ManifestEntry> manifest;

    for (const auto& base_path : config_paths()) {
        std::error_code ec;
        if (!std::filesystem::exists(base_path, ec)) continue;

        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(base_path, ec)) {
            if (ec) break;
            if (!entry.is_regular_file()) continue;

            ManifestEntry me;
            me.path = entry.path().string();
            me.size = entry.file_size(ec);
            me.mtime = file_mtime(entry.path());

            auto h = hash_file(entry.path());
            if (h.has_value()) {
                me.sha256 = h.value();
            }

            manifest.push_back(std::move(me));
        }
    }

    // Also capture package list
    {
        ManifestEntry pkg;
        pkg.path = "__packages__";
        // Capture dpkg selections as a virtual file
        FILE* pipe = popen("dpkg --get-selections 2>/dev/null || "
                           "rpm -qa 2>/dev/null || "
                           "brew list --formula 2>/dev/null", "r");
        if (pipe) {
            std::string content;
            char buf[4096];
            while (fgets(buf, sizeof(buf), pipe)) {
                content += buf;
            }
            pclose(pipe);

            SHA256_CTX ctx;
            SHA256_Init(&ctx);
            SHA256_Update(&ctx, content.data(), content.size());
            unsigned char hash[SHA256_DIGEST_LENGTH];
            SHA256_Final(hash, &ctx);

            std::ostringstream oss;
            oss << std::hex << std::setfill('0');
            for (size_t i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
                oss << std::setw(2) << static_cast<int>(hash[i]);
            }
            pkg.sha256 = oss.str();
            pkg.size = content.size();
            manifest.push_back(std::move(pkg));
        }
    }

    return Result<std::vector<ManifestEntry>, std::string>::ok(std::move(manifest));
}

// ---------------------------------------------------------------------------
// Comparison
// ---------------------------------------------------------------------------

DiffSummary DiffEngine::compare(const std::vector<ManifestEntry>& local,
                                 const std::vector<ManifestEntry>& remote) const {
    DiffSummary diff;

    std::map<std::string, const ManifestEntry*> local_map;
    for (const auto& e : local) local_map[e.path] = &e;

    std::map<std::string, const ManifestEntry*> remote_map;
    for (const auto& e : remote) remote_map[e.path] = &e;

    // Files in local
    for (const auto& [path, le] : local_map) {
        FileDiff fd;
        fd.path = path;
        fd.local_size = le->size;
        fd.local_hash = le->sha256;
        fd.local_mtime = le->mtime;

        auto rit = remote_map.find(path);
        if (rit == remote_map.end()) {
            fd.status = "added";
            fd.transfer_bytes = le->size;
            diff.added++;
        } else {
            fd.remote_size = rit->second->size;
            fd.remote_hash = rit->second->sha256;
            fd.remote_mtime = rit->second->mtime;

            if (le->sha256 == rit->second->sha256) {
                fd.status = "unchanged";
                diff.unchanged++;
            } else {
                fd.status = "modified";
                fd.transfer_bytes = le->size;
                diff.modified++;
            }
        }
        diff.files.push_back(std::move(fd));
        diff.transfer_bytes += fd.transfer_bytes;
    }

    // Files only in remote (would be removed on sync)
    for (const auto& [path, re] : remote_map) {
        if (local_map.count(path) == 0) {
            FileDiff fd;
            fd.path = path;
            fd.status = "removed";
            fd.remote_size = re->size;
            fd.remote_hash = re->sha256;
            fd.remote_mtime = re->mtime;
            diff.removed++;
            diff.files.push_back(std::move(fd));
        }
    }

    // Sort by status then path
    std::sort(diff.files.begin(), diff.files.end(), [](const FileDiff& a, const FileDiff& b) {
        if (a.status != b.status) return a.status < b.status;
        return a.path < b.path;
    });

    return diff;
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

std::string DiffEngine::manifest_to_json(const std::vector<ManifestEntry>& manifest) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& e : manifest) {
        nlohmann::json j;
        j["path"] = e.path;
        j["size"] = e.size;
        j["sha256"] = e.sha256;
        j["mtime"] = e.mtime;
        arr.push_back(std::move(j));
    }
    return arr.dump(2);
}

Result<std::vector<ManifestEntry>, std::string> DiffEngine::manifest_from_json(
    const std::string& json_str)
{
    nlohmann::json arr;
    try {
        arr = nlohmann::json::parse(json_str);
    } catch (const nlohmann::json::parse_error& e) {
        return Result<std::vector<ManifestEntry>, std::string>::error(
            std::string("Manifest parse error: ") + e.what());
    }

    if (!arr.is_array()) {
        return Result<std::vector<ManifestEntry>, std::string>::error(
            "Manifest must be a JSON array");
    }

    std::vector<ManifestEntry> manifest;
    for (const auto& j : arr) {
        ManifestEntry e;
        e.path = j.value("path", "");
        e.size = j.value("size", uint64_t(0));
        e.sha256 = j.value("sha256", "");
        e.mtime = j.value("mtime", "");
        manifest.push_back(std::move(e));
    }

    return Result<std::vector<ManifestEntry>, std::string>::ok(std::move(manifest));
}

// ---------------------------------------------------------------------------
// Formatting
// ---------------------------------------------------------------------------

std::string DiffEngine::format_diff(const DiffSummary& diff) {
    std::ostringstream oss;
    oss << "\nSystem Differences:\n\n";

    auto pad = [](const std::string& s, size_t w) -> std::string {
        return (s.size() >= w) ? s : s + std::string(w - s.size(), ' ');
    };

    auto format_size = [](uint64_t bytes) -> std::string {
        if (bytes < 1024) return std::to_string(bytes) + " B";
        if (bytes < 1024 * 1024) return std::to_string(bytes / 1024) + " KB";
        return std::to_string(bytes / (1024 * 1024)) + " MB";
    };

    oss << pad("STATUS", 12) << pad("SIZE", 12) << "PATH\n";
    oss << std::string(80, '-') << "\n";

    for (const auto& f : diff.files) {
        if (f.status == "unchanged") continue; // Only show changes
        std::string status_marker;
        if (f.status == "added") status_marker = "[+] added";
        else if (f.status == "removed") status_marker = "[-] removed";
        else if (f.status == "modified") status_marker = "[~] modified";

        uint64_t size = (f.local_size > 0) ? f.local_size : f.remote_size;
        oss << pad(status_marker, 12) << pad(format_size(size), 12) << f.path << "\n";
    }

    oss << "\nSummary:\n";
    oss << "  Added:     " << diff.added << "\n";
    oss << "  Removed:   " << diff.removed << "\n";
    oss << "  Modified:  " << diff.modified << "\n";
    oss << "  Unchanged: " << diff.unchanged << "\n";
    oss << "  Transfer:  " << format_size(diff.transfer_bytes) << "\n";

    return oss.str();
}

} // namespace straylight
