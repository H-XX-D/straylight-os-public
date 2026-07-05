// tools/link/link_manager.cpp
// Full implementation of intelligent symlink and resource management.

#include "link_manager.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace straylight {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

LinkManager::LinkManager() {
    auto r = load_registry();
    (void)r;
}

LinkManager::~LinkManager() {
    if (registry_loaded_) {
        auto r = save_registry();
        (void)r;
    }
}

std::string LinkManager::home_dir() const {
    const char* h = std::getenv("HOME");
    return h ? std::string(h) : "/root";
}

std::string LinkManager::registry_path() const {
    return home_dir() + "/" + kRegistryDir + "/" + kRegistryFile;
}

Result<void, std::string> LinkManager::load_registry() {
    std::string path = registry_path();
    std::ifstream in(path);
    if (!in.is_open()) {
        // No registry yet -- that is fine.
        registry_loaded_ = true;
        return Result<void, std::string>::ok();
    }

    links_.clear();

    // Minimal hand-rolled JSON array parser (no external deps in tools).
    std::string content((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    in.close();

    // Each entry looks like:
    // { "link": "...", "target": "...", "owner": "...", "tag": "...",
    //   "created_epoch": N, "alive": true/false }
    // We scan for '{' ... '}' blocks.
    size_t pos = 0;
    while (pos < content.size()) {
        auto obj_start = content.find('{', pos);
        if (obj_start == std::string::npos) break;
        auto obj_end = content.find('}', obj_start);
        if (obj_end == std::string::npos) break;

        std::string block = content.substr(obj_start, obj_end - obj_start + 1);
        pos = obj_end + 1;

        // Extract fields.
        auto extract_str = [&](const std::string& key) -> std::string {
            std::string needle = "\"" + key + "\"";
            auto kp = block.find(needle);
            if (kp == std::string::npos) return {};
            auto colon = block.find(':', kp + needle.size());
            if (colon == std::string::npos) return {};
            auto q1 = block.find('"', colon + 1);
            auto q2 = block.find('"', q1 + 1);
            if (q1 == std::string::npos || q2 == std::string::npos) return {};
            return block.substr(q1 + 1, q2 - q1 - 1);
        };
        auto extract_long = [&](const std::string& key) -> long long {
            std::string needle = "\"" + key + "\"";
            auto kp = block.find(needle);
            if (kp == std::string::npos) return 0;
            auto colon = block.find(':', kp + needle.size());
            if (colon == std::string::npos) return 0;
            return std::atoll(block.c_str() + colon + 1);
        };
        auto extract_bool = [&](const std::string& key) -> bool {
            std::string needle = "\"" + key + "\"";
            auto kp = block.find(needle);
            if (kp == std::string::npos) return true;
            return block.find("true", kp) != std::string::npos &&
                   block.find("true", kp) < block.find("false", kp);
        };

        ManagedLink ml;
        ml.link_path = extract_str("link");
        ml.target_path = extract_str("target");
        ml.owner = extract_str("owner");
        ml.tag = extract_str("tag");
        ml.alive = extract_bool("alive");
        auto ep = extract_long("created_epoch");
        if (ep > 0) {
            ml.created = std::chrono::system_clock::from_time_t(
                static_cast<time_t>(ep));
        }
        if (!ml.link_path.empty()) {
            links_.push_back(std::move(ml));
        }
    }

    registry_loaded_ = true;
    return Result<void, std::string>::ok();
}

Result<void, std::string> LinkManager::save_registry() const {
    std::string dir = home_dir() + "/" + kRegistryDir;
    std::error_code ec;
    fs::create_directories(dir, ec);

    std::string path = registry_path();
    std::ofstream out(path);
    if (!out.is_open()) {
        return Result<void, std::string>::error(
            "cannot write registry: " + path);
    }

    out << "[\n";
    for (size_t i = 0; i < links_.size(); ++i) {
        const auto& ml = links_[i];
        auto epoch = std::chrono::duration_cast<std::chrono::seconds>(
                         ml.created.time_since_epoch())
                         .count();
        out << "  {\n";
        out << "    \"link\": \"" << ml.link_path << "\",\n";
        out << "    \"target\": \"" << ml.target_path << "\",\n";
        out << "    \"owner\": \"" << ml.owner << "\",\n";
        out << "    \"tag\": \"" << ml.tag << "\",\n";
        out << "    \"created_epoch\": " << epoch << ",\n";
        out << "    \"alive\": " << (ml.alive ? "true" : "false") << "\n";
        out << "  }";
        if (i + 1 < links_.size()) out << ",";
        out << "\n";
    }
    out << "]\n";
    out.close();

    return Result<void, std::string>::ok();
}

void LinkManager::register_link(const ManagedLink& link) {
    // Replace if same link_path already exists.
    for (auto& existing : links_) {
        if (existing.link_path == link.link_path) {
            existing = link;
            return;
        }
    }
    links_.push_back(link);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

Result<LinkManager::ManagedLink, std::string>
LinkManager::create(const std::string& target, const std::string& link_path,
                     const std::string& tag) {
    if (target.empty() || link_path.empty()) {
        return Result<ManagedLink, std::string>::error(
            "target and link path must not be empty");
    }

    // Resolve the target to an absolute path.
    std::error_code ec;
    std::string abs_target = fs::absolute(target, ec).string();
    if (ec) {
        abs_target = target; // Use as-is if resolution fails.
    }

    // Check target exists.
    if (!fs::exists(abs_target, ec)) {
        return Result<ManagedLink, std::string>::error(
            "target does not exist: " + abs_target);
    }

    std::string abs_link = fs::absolute(link_path, ec).string();
    if (ec) {
        abs_link = link_path;
    }

    // If the link already exists, check if it is a symlink we can replace.
    if (fs::exists(abs_link, ec) || fs::is_symlink(abs_link, ec)) {
        if (fs::is_symlink(abs_link, ec)) {
            fs::remove(abs_link, ec);
            if (ec) {
                return Result<ManagedLink, std::string>::error(
                    "cannot remove existing symlink: " + ec.message());
            }
        } else {
            return Result<ManagedLink, std::string>::error(
                "link path already exists and is not a symlink: " + abs_link);
        }
    }

    // Ensure parent directory exists.
    fs::create_directories(fs::path(abs_link).parent_path(), ec);

    // Create the symlink.
    fs::create_symlink(abs_target, abs_link, ec);
    if (ec) {
        return Result<ManagedLink, std::string>::error(
            "symlink creation failed: " + ec.message());
    }

    ManagedLink ml;
    ml.link_path = abs_link;
    ml.target_path = abs_target;
    ml.owner = home_dir();
    ml.tag = tag;
    ml.created = std::chrono::system_clock::now();
    ml.alive = true;

    register_link(ml);
    auto save_res = save_registry();
    if (!save_res.has_value()) {
        // Non-fatal: symlink was created but registry write failed.
    }

    return Result<ManagedLink, std::string>::ok(std::move(ml));
}

Result<std::vector<LinkManager::ManagedLink>, std::string>
LinkManager::dotfiles(const std::string& repo_dir) {
    std::error_code ec;
    if (!fs::is_directory(repo_dir, ec)) {
        return Result<std::vector<ManagedLink>, std::string>::error(
            "not a directory: " + repo_dir);
    }

    std::string home = home_dir();
    std::vector<ManagedLink> created;

    // Walk the repo directory. For each file/directory that starts with '.'
    // (or is inside a directory that starts with '.'), create a symlink in $HOME.
    for (auto& entry : fs::directory_iterator(repo_dir, ec)) {
        std::string name = entry.path().filename().string();

        // Skip .git directory and other VCS metadata.
        if (name == ".git" || name == ".gitignore" || name == ".gitmodules") {
            continue;
        }

        // Determine the destination path in $HOME.
        std::string dest;
        if (name[0] == '.') {
            // Already a dotfile name, link directly.
            dest = home + "/" + name;
        } else {
            // Convention: plain name maps to ~/.name
            dest = home + "/." + name;
        }

        std::string target = fs::absolute(entry.path(), ec).string();

        // If destination already exists and is not a symlink, skip to avoid data loss.
        if (fs::exists(dest, ec) && !fs::is_symlink(dest, ec)) {
            std::cerr << "  skip: " << dest << " (exists, not a symlink)\n";
            continue;
        }

        auto res = create(target, dest, "dotfiles");
        if (res.has_value()) {
            created.push_back(res.value());
        } else {
            std::cerr << "  warn: " << dest << ": " << res.error() << "\n";
        }
    }

    return Result<std::vector<ManagedLink>, std::string>::ok(std::move(created));
}

Result<std::vector<std::string>, std::string>
LinkManager::audit(const std::string& root) const {
    std::vector<std::string> broken;
    std::error_code ec;

    // Walk the filesystem looking for broken symlinks.
    // We limit depth to avoid extremely long scans.
    constexpr int kMaxDepth = 8;

    std::function<void(const fs::path&, int)> walk;
    walk = [&](const fs::path& dir, int depth) {
        if (depth > kMaxDepth) return;

        for (auto& entry : fs::directory_iterator(dir,
                 fs::directory_options::skip_permission_denied, ec)) {
            if (ec) { ec.clear(); continue; }

            // Skip /proc, /sys, /dev to avoid noise.
            std::string p = entry.path().string();
            if (p == "/proc" || p == "/sys" || p == "/dev" || p == "/run") {
                continue;
            }

            if (entry.is_symlink(ec)) {
                // Check if the symlink target exists.
                auto target = fs::read_symlink(entry.path(), ec);
                if (ec) { ec.clear(); continue; }

                fs::path resolved = target.is_absolute()
                    ? target
                    : entry.path().parent_path() / target;

                if (!fs::exists(resolved, ec)) {
                    broken.push_back(entry.path().string() + " -> " +
                                     target.string());
                }
                ec.clear();
            } else if (entry.is_directory(ec) && !entry.is_symlink(ec)) {
                walk(entry.path(), depth + 1);
            }
            ec.clear();
        }
        ec.clear();
    };

    walk(fs::path(root), 0);

    // Also check all managed links.
    for (const auto& ml : links_) {
        if (!fs::is_symlink(ml.link_path, ec)) {
            broken.push_back(ml.link_path + " [managed, missing]");
        } else {
            auto target = fs::read_symlink(ml.link_path, ec);
            if (ec) continue;
            fs::path resolved = target.is_absolute()
                ? target
                : fs::path(ml.link_path).parent_path() / target;
            if (!fs::exists(resolved, ec)) {
                broken.push_back(ml.link_path + " -> " + target.string() +
                                 " [managed, broken]");
            }
        }
        ec.clear();
    }

    // Deduplicate.
    std::sort(broken.begin(), broken.end());
    broken.erase(std::unique(broken.begin(), broken.end()), broken.end());

    return Result<std::vector<std::string>, std::string>::ok(std::move(broken));
}

Result<std::string, std::string> LinkManager::graph() const {
    if (links_.empty()) {
        return Result<std::string, std::string>::ok("(no managed links)\n");
    }

    // Build adjacency: target -> [links]
    std::map<std::string, std::vector<std::string>> adj;
    for (const auto& ml : links_) {
        adj[ml.target_path].push_back(ml.link_path);
    }

    std::ostringstream out;
    out << "StrayLight Link Graph\n";
    out << std::string(40, '=') << "\n\n";

    for (const auto& [target, symlinks] : adj) {
        // Check if this target is itself a managed link.
        bool target_is_link = false;
        for (const auto& ml : links_) {
            if (ml.link_path == target) {
                target_is_link = true;
                break;
            }
        }

        out << (target_is_link ? "[link] " : "[file] ") << target << "\n";
        for (size_t i = 0; i < symlinks.size(); ++i) {
            bool last = (i + 1 == symlinks.size());
            out << (last ? "  +-- " : "  |-- ") << symlinks[i];

            // Find the tag for this link.
            for (const auto& ml : links_) {
                if (ml.link_path == symlinks[i] && !ml.tag.empty()) {
                    out << " [" << ml.tag << "]";
                    break;
                }
            }

            // Check if alive.
            std::error_code ec;
            if (!fs::exists(symlinks[i], ec)) {
                out << " (BROKEN)";
            }
            out << "\n";
        }
        out << "\n";
    }

    return Result<std::string, std::string>::ok(out.str());
}

Result<void, std::string>
LinkManager::watch(const std::string& dir, const std::string& pattern,
                    const std::string& dest_dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return Result<void, std::string>::error(
            "not a directory: " + dir);
    }

    std::string destination = dest_dir.empty() ? home_dir() : dest_dir;
    fs::create_directories(destination, ec);

    // Use inotify to watch for new files.
    int ifd = inotify_init();
    if (ifd < 0) {
        return Result<void, std::string>::error(
            "inotify_init failed: " + std::string(strerror(errno)));
    }

    int wd = inotify_add_watch(ifd, dir.c_str(),
                                IN_CREATE | IN_MOVED_TO);
    if (wd < 0) {
        close(ifd);
        return Result<void, std::string>::error(
            "inotify_add_watch failed: " + std::string(strerror(errno)));
    }

    std::cout << "Watching " << dir << " for pattern '" << pattern
              << "', linking to " << destination << "\n"
              << "Press Ctrl+C to stop.\n";

    // Simple glob matching: '*' matches anything, '?' matches one char.
    auto glob_match = [](const std::string& pat, const std::string& str) -> bool {
        size_t pi = 0, si = 0;
        size_t star_pi = std::string::npos, star_si = 0;
        while (si < str.size()) {
            if (pi < pat.size() && (pat[pi] == '?' || pat[pi] == str[si])) {
                ++pi;
                ++si;
            } else if (pi < pat.size() && pat[pi] == '*') {
                star_pi = pi++;
                star_si = si;
            } else if (star_pi != std::string::npos) {
                pi = star_pi + 1;
                si = ++star_si;
            } else {
                return false;
            }
        }
        while (pi < pat.size() && pat[pi] == '*') ++pi;
        return pi == pat.size();
    };

    constexpr size_t kBufSize = 4096;
    char buf[kBufSize];

    while (true) {
        ssize_t len = read(ifd, buf, kBufSize);
        if (len <= 0) break;

        size_t offset = 0;
        while (offset < static_cast<size_t>(len)) {
            auto* event = reinterpret_cast<struct inotify_event*>(buf + offset);
            offset += sizeof(struct inotify_event) + event->len;

            if (event->len == 0) continue;
            std::string name(event->name);

            if (!glob_match(pattern, name)) continue;

            std::string target = dir + "/" + name;
            std::string link = destination + "/" + name;

            auto res = create(target, link, "watch");
            if (res.has_value()) {
                std::cout << "  linked: " << link << " -> " << target << "\n";
            } else {
                std::cerr << "  error: " << name << ": " << res.error() << "\n";
            }
        }
    }

    close(ifd);
    return Result<void, std::string>::ok();
}

Result<int, std::string> LinkManager::restore() {
    int restored = 0;
    int failed = 0;
    std::error_code ec;

    for (auto& ml : links_) {
        // Check if symlink already exists and is correct.
        if (fs::is_symlink(ml.link_path, ec)) {
            auto existing_target = fs::read_symlink(ml.link_path, ec);
            if (!ec && existing_target.string() == ml.target_path) {
                ml.alive = true;
                continue; // Already correct.
            }
            // Remove incorrect symlink.
            fs::remove(ml.link_path, ec);
        }

        // Check that target still exists.
        if (!fs::exists(ml.target_path, ec)) {
            ml.alive = false;
            ++failed;
            std::cerr << "  skip (target missing): " << ml.link_path
                      << " -> " << ml.target_path << "\n";
            continue;
        }

        // Ensure parent directory exists.
        fs::create_directories(fs::path(ml.link_path).parent_path(), ec);

        // Recreate the symlink.
        fs::create_symlink(ml.target_path, ml.link_path, ec);
        if (ec) {
            ml.alive = false;
            ++failed;
            std::cerr << "  fail: " << ml.link_path << ": " << ec.message() << "\n";
        } else {
            ml.alive = true;
            ++restored;
        }
    }

    auto save_res = save_registry();
    (void)save_res;

    if (failed > 0) {
        return Result<int, std::string>::ok(restored);
    }
    return Result<int, std::string>::ok(restored);
}

std::vector<LinkManager::ManagedLink> LinkManager::list() const {
    return links_;
}

Result<void, std::string>
LinkManager::remove(const std::string& link_path, bool delete_symlink) {
    auto it = std::find_if(links_.begin(), links_.end(),
                           [&](const ManagedLink& ml) {
                               return ml.link_path == link_path;
                           });
    if (it == links_.end()) {
        return Result<void, std::string>::error(
            "link not found in registry: " + link_path);
    }

    if (delete_symlink) {
        std::error_code ec;
        fs::remove(link_path, ec);
        // Ignore error -- symlink might already be gone.
    }

    links_.erase(it);
    auto save_res = save_registry();
    if (!save_res.has_value()) {
        return Result<void, std::string>::error(
            "link removed but registry save failed: " + save_res.error());
    }

    return Result<void, std::string>::ok();
}

} // namespace straylight
